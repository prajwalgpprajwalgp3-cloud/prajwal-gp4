/*
 * Real-Time Chart Data Server
 * Concurrent HTTP + WebSocket server in C
 * Uses POSIX threads for concurrency, epoll for I/O multiplexing
 */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <fcntl.h>
#include <arpa/inet.h>

/* ─── Configuration ─────────────────────────────────────── */
#define PORT            8080
#define MAX_EVENTS      64
#define MAX_CLIENTS     256
#define BACKLOG         16
#define WORKER_THREADS  4
#define DATA_CHANNELS   4
#define HISTORY_LEN     120   /* 120 data points per channel */
#define TICK_MS         500   /* emit new data every 500 ms  */

/* ─── WebSocket frame opcodes ───────────────────────────── */
#define WS_OP_TEXT   0x1
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* ─── Client state ──────────────────────────────────────── */
typedef enum { STATE_HTTP, STATE_WS } ClientState;

typedef struct {
    int      fd;
    ClientState state;
    char     buf[4096];
    int      buf_len;
} Client;

/* ─── Globals ────────────────────────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static int    g_epoll_fd = -1;
static int    g_server_fd = -1;

/* Client table (fd-indexed) */
static Client  g_clients[MAX_CLIENTS];
static pthread_mutex_t g_clients_mu = PTHREAD_MUTEX_INITIALIZER;

/* Circular data history */
typedef struct {
    double  val[HISTORY_LEN];
    int     head;   /* next write position */
    int     count;
} Channel;

static Channel g_channels[DATA_CHANNELS];
static pthread_rwlock_t g_data_rw = PTHREAD_RWLOCK_INITIALIZER;



/* ─── Embedded HTML dashboard (single-file) ─────────────── */
/* (Stored as a long string; served via HTTP GET /)           */
static const char *DASHBOARD_HTML =
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>RT Chart Server</title>"
"<script src='https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js'></script>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0d0f14;color:#e2e4ea;font-family:'Courier New',monospace;padding:24px}"
"h1{font-size:1.1rem;letter-spacing:.18em;color:#7b8cde;margin-bottom:20px;text-transform:uppercase}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px}"
".card{background:#13161f;border:1px solid #1e2230;border-radius:8px;padding:16px}"
".card h2{font-size:.72rem;letter-spacing:.15em;color:#7b8cde;margin-bottom:12px;text-transform:uppercase}"
".stat{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px}"
".val{font-size:1.6rem;font-weight:700;color:#e2e4ea}"
".unit{font-size:.75rem;color:#555e7a;margin-left:4px}"
".delta{font-size:.75rem}"
".up{color:#3ddc97}.dn{color:#e05c5c}"
"canvas{max-height:140px}"
"#status{position:fixed;top:16px;right:20px;font-size:.68rem;letter-spacing:.1em;padding:4px 10px;"
"border-radius:4px;border:1px solid #1e2230;background:#13161f;color:#555e7a}"
"#status.live{color:#3ddc97;border-color:#3ddc97}"
"</style>"
"</head>"
"<body>"
"<div id='status'>CONNECTING…</div>"
"<h1>&#9632; Real-Time Server Monitor</h1>"
"<div class='grid' id='grid'></div>"
"<script>"
"const NAMES=['CPU Load','Memory','Network I/O','Disk I/O'];"
"const COLORS=['#7b8cde','#3ddc97','#f0a04b','#e05c5c'];"
"const charts=[];"
"function mkCard(i){"
"  const d=document.createElement('div');d.className='card';d.id='c'+i;"
"  d.innerHTML=`<h2>${NAMES[i]}</h2>`"
"    +`<div class='stat'><span class='val' id='v${i}'>--</span>`"
"    +`<span class='delta' id='d${i}'></span></div>`"
"    +`<canvas id='ch${i}'></canvas>`;"
"  document.getElementById('grid').appendChild(d);"
"  const ctx=document.getElementById('ch'+i).getContext('2d');"
"  charts[i]=new Chart(ctx,{"
"    type:'line',"
"    data:{labels:[],datasets:[{data:[],borderColor:COLORS[i],borderWidth:1.5,"
"      backgroundColor:COLORS[i]+'22',fill:true,tension:.35,pointRadius:0}]},"
"    options:{animation:false,responsive:true,maintainAspectRatio:false,"
"      scales:{x:{display:false},y:{min:0,max:100,display:true,"
"        ticks:{color:'#555e7a',font:{size:10},callback:v=>v+'%'},"
"        grid:{color:'#1e2230'}}},"
"      plugins:{legend:{display:false},tooltip:{enabled:false}}}});"
"}"
"for(let i=0;i<4;i++)mkCard(i);"
"function connect(){"
"  const ws=new WebSocket(`ws://${location.host}/ws`);"
"  ws.onopen=()=>{document.getElementById('status').textContent='● LIVE';"
"    document.getElementById('status').className='live';};"
"  ws.onclose=()=>{document.getElementById('status').textContent='RECONNECTING…';"
"    document.getElementById('status').className='';"
"    setTimeout(connect,1500);};"
"  ws.onmessage=e=>{"
"    const msg=JSON.parse(e.data);"
"    if(msg.type==='history'){"
"      msg.channels.forEach((ch,i)=>{"
"        const c=charts[i];c.data.labels=ch.map((_,j)=>j);"
"        c.data.datasets[0].data=ch;c.update('none');"
"        const last=ch[ch.length-1];"
"        document.getElementById('v'+i).textContent=last.toFixed(1)+"
"          '<span class=unit>%</span>';"
"      });"
"    } else if(msg.type==='tick'){"
"      msg.values.forEach((v,i)=>{"
"        const c=charts[i];const prev=c.data.datasets[0].data.slice(-1)[0]||0;"
"        c.data.labels.push('');if(c.data.labels.length>120)c.data.labels.shift();"
"        c.data.datasets[0].data.push(v);"
"        if(c.data.datasets[0].data.length>120)c.data.datasets[0].data.shift();"
"        c.update('none');"
"        const el=document.getElementById('v'+i);"
"        el.innerHTML=v.toFixed(1)+'<span class=unit>%</span>';"
"        const de=document.getElementById('d'+i);"
"        const diff=v-prev;de.textContent=(diff>=0?'+':'')+diff.toFixed(1)+'%';"
"        de.className='delta '+(diff>=0?'up':'dn');"
"      });"
"    }"
"  };"
"}"
"connect();"
"</script>"
"</body></html>";

/* ─── Utility: set fd non-blocking ──────────────────────── */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ─── Simple SHA1 for WebSocket handshake ───────────────── */
/* Minimal FIPS-180-4 SHA-1 implementation (no external deps) */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; int buf_pos; } SHA1Ctx;

static void sha1_init(SHA1Ctx *c) {
    c->h[0]=0x67452301; c->h[1]=0xEFCDAB89;
    c->h[2]=0x98BADCFE; c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0;
    c->len=0; c->buf_pos=0;
}
#define ROL32(x,n) (((x)<<(n))|((x)>>(32-(n))))
static void sha1_compress(SHA1Ctx *c) {
    uint32_t w[80]; uint8_t *b=c->buf;
    for(int i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for(int i=16;i<80;i++) w[i]=ROL32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=c->h[0],b2=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4];
    for(int i=0;i<80;i++){
        uint32_t f,k;
        if(i<20){f=(b2&cc)|(~b2&d);k=0x5A827999;}
        else if(i<40){f=b2^cc^d;k=0x6ED9EBA1;}
        else if(i<60){f=(b2&cc)|(b2&d)|(cc&d);k=0x8F1BBCDC;}
        else{f=b2^cc^d;k=0xCA62C1D6;}
        uint32_t tmp=ROL32(a,5)+f+e+k+w[i];
        e=d;d=cc;cc=ROL32(b2,30);b2=a;a=tmp;
    }
    c->h[0]+=a;c->h[1]+=b2;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;
}
static void sha1_update(SHA1Ctx *c, const uint8_t *data, size_t len) {
    for(size_t i=0;i<len;i++){
        c->buf[c->buf_pos++]=data[i]; c->len++;
        if(c->buf_pos==64){sha1_compress(c);c->buf_pos=0;}
    }
}
static void sha1_final(SHA1Ctx *c, uint8_t out[20]) {
    uint8_t tmp=0x80;
    sha1_update(c,&tmp,1);
    while(c->buf_pos!=56){uint8_t z=0;sha1_update(c,&z,1);}
    uint64_t bits=c->len*8;
    for(int i=7;i>=0;i--){uint8_t x=(bits>>(i*8))&0xFF;sha1_update(c,&x,1);}
    for(int i=0;i<5;i++){
        out[i*4+0]=(c->h[i]>>24)&0xFF; out[i*4+1]=(c->h[i]>>16)&0xFF;
        out[i*4+2]=(c->h[i]>>8)&0xFF;  out[i*4+3]=c->h[i]&0xFF;
    }
}

/* Base64 encode */
static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int base64_encode(const uint8_t *in, int in_len, char *out) {
    int i=0,o=0;
    while(i<in_len){
        uint32_t v=(uint32_t)in[i]<<16; if(i+1<in_len)v|=(uint32_t)in[i+1]<<8; if(i+2<in_len)v|=in[i+2];
        out[o++]=B64[(v>>18)&63]; out[o++]=B64[(v>>12)&63];
        out[o++]=(i+1<in_len)?B64[(v>>6)&63]:'=';
        out[o++]=(i+2<in_len)?B64[v&63]:'=';
        i+=3;
    }
    out[o]='\0'; return o;
}

/* ─── WebSocket upgrade ─────────────────────────────────── */
static int ws_upgrade(int fd, const char *key) {
    /* Compute accept key */
    char magic[256];
    snprintf(magic,sizeof(magic),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    SHA1Ctx ctx; sha1_init(&ctx);
    sha1_update(&ctx,(uint8_t*)magic,strlen(magic));
    uint8_t hash[20]; sha1_final(&ctx,hash);
    char accept[64]; base64_encode(hash,20,accept);

    char resp[512];
    int n=snprintf(resp,sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    send(fd,resp,n,MSG_NOSIGNAL);
    return 0;
}

/* ─── WebSocket frame send (text, unmasked) ─────────────── */
static void ws_send(int fd, const char *payload, size_t len) {
    uint8_t hdr[10]; int hlen=2;
    hdr[0]=0x80|WS_OP_TEXT;
    if(len<=125){hdr[1]=(uint8_t)len;}
    else if(len<=65535){hdr[1]=126;hdr[2]=(len>>8)&0xFF;hdr[3]=len&0xFF;hlen=4;}
    else{hdr[1]=127;for(int i=0;i<8;i++)hdr[2+i]=(len>>((7-i)*8))&0xFF;hlen=10;}
    send(fd,hdr,hlen,MSG_NOSIGNAL);
    send(fd,payload,len,MSG_NOSIGNAL);
}

/* Broadcast to all WS clients */
static void ws_broadcast(const char *payload, size_t len) {
    pthread_mutex_lock(&g_clients_mu);
    for(int i=3;i<MAX_CLIENTS;i++){
        if(g_clients[i].fd>0 && g_clients[i].state==STATE_WS)
            ws_send(i,payload,len);
    }
    pthread_mutex_unlock(&g_clients_mu);
}

/* ─── Send history snapshot to a new WS client ─────────── */
static void send_history(int fd) {
    char json[8192];
    int pos=snprintf(json,sizeof(json),"{\"type\":\"history\",\"channels\":[");
    pthread_rwlock_rdlock(&g_data_rw);
    for(int c=0;c<DATA_CHANNELS;c++){
        pos+=snprintf(json+pos,sizeof(json)-pos,"[");
        Channel *ch=&g_channels[c];
        int start=(ch->count<HISTORY_LEN)?0:(ch->head);
        int n=(ch->count<HISTORY_LEN)?ch->count:HISTORY_LEN;
        for(int i=0;i<n;i++){
            int idx=(start+i)%HISTORY_LEN;
            pos+=snprintf(json+pos,sizeof(json)-pos,"%.2f%s",ch->val[idx],(i<n-1)?",":"");
        }
        pos+=snprintf(json+pos,sizeof(json)-pos,"]%s",(c<DATA_CHANNELS-1)?",":"");
    }
    pthread_rwlock_unlock(&g_data_rw);
    pos+=snprintf(json+pos,sizeof(json)-pos,"]}");
    ws_send(fd,json,(size_t)pos);
}

/* ─── Data generation thread ────────────────────────────── */
static double g_phase[DATA_CHANNELS]={0.0,1.0,2.0,3.0};
static double g_noise[DATA_CHANNELS]={0.0,0.0,0.0,0.0};

static void* data_thread(void *arg) {
    (void)arg;
    struct timespec ts={0, TICK_MS*1000000L};

    while(g_running){
        nanosleep(&ts,NULL);

        /* Generate simulated metric values (sine + random walk) */
        double vals[DATA_CHANNELS];
        pthread_rwlock_wrlock(&g_data_rw);
        for(int c=0;c<DATA_CHANNELS;c++){
            /* slow sine wave + noise */
            g_phase[c]+=0.04+(c*0.01);
            double rnd=((double)rand()/RAND_MAX*2.0-1.0)*3.5;
            g_noise[c]=g_noise[c]*0.85+rnd*0.15;
            double v=45.0+35.0*sin(g_phase[c])+g_noise[c]*8.0;
            /* Clamp to [1,99] */
            if(v<1){v=1;} if(v>99){v=99;}
            vals[c]=v;

            Channel *ch=&g_channels[c];
            ch->val[ch->head]=v;
            ch->head=(ch->head+1)%HISTORY_LEN;
            if(ch->count<HISTORY_LEN)ch->count++;
        }
        pthread_rwlock_unlock(&g_data_rw);

        /* Build tick JSON */
        char json[512];
        int pos=snprintf(json,sizeof(json),"{\"type\":\"tick\",\"values\":[");
        for(int c=0;c<DATA_CHANNELS;c++)
            pos+=snprintf(json+pos,sizeof(json)-pos,"%.2f%s",vals[c],(c<DATA_CHANNELS-1)?",":"");
        pos+=snprintf(json+pos,sizeof(json)-pos,"]}");
        ws_broadcast(json,(size_t)pos);
    }
    return NULL;
}

/* ─── Handle incoming HTTP request ─────────────────────── */
static void handle_http(Client *cl) {
    char *req=cl->buf;

    /* Parse Sec-WebSocket-Key header */
    char *ws_key=strstr(req,"Sec-WebSocket-Key:");
    if(ws_key){
        ws_key+=18;
        while(*ws_key==' ')ws_key++;
        char key[128]={0};
        char *end=strstr(ws_key,"\r\n");
        if(end){int kl=(int)(end-ws_key);if(kl>127)kl=127;memcpy(key,ws_key,kl);}
        ws_upgrade(cl->fd,key);
        cl->state=STATE_WS;
        send_history(cl->fd);
        printf("[ws ] client fd=%d upgraded\n",cl->fd);
        return;
    }

    /* Serve dashboard HTML */
    if(strncmp(req,"GET / ",6)==0 || strncmp(req,"GET /index",10)==0){
        size_t hlen=strlen(DASHBOARD_HTML);
        char hdr[256];
        int n=snprintf(hdr,sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n", hlen);
        send(cl->fd,hdr,n,MSG_NOSIGNAL);
        send(cl->fd,DASHBOARD_HTML,hlen,MSG_NOSIGNAL);
    } else {
        const char *r404="HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        send(cl->fd,r404,strlen(r404),MSG_NOSIGNAL);
    }

    /* Close non-WS connections */
    close(cl->fd); cl->fd=-1; cl->state=STATE_HTTP;
}

/* ─── Handle WebSocket frame (unmask + dispatch) ────────── */
static void handle_ws_frame(Client *cl) {
    uint8_t *d=(uint8_t*)cl->buf;
    if(cl->buf_len<2)return;
    uint8_t opcode=d[0]&0x0F;
    int masked=(d[1]&0x80)?1:0;
    int pay_len=d[1]&0x7F;
    int hdr_off=2;
    if(pay_len==126){if(cl->buf_len<4)return;pay_len=(d[2]<<8)|d[3];hdr_off=4;}
    if(!masked){cl->buf_len=0;return;}
    if(cl->buf_len<hdr_off+4+pay_len)return;
    uint8_t mask[4]; memcpy(mask,d+hdr_off,4); hdr_off+=4;
    uint8_t *pay=d+hdr_off;
    for(int i=0;i<pay_len;i++)pay[i]^=mask[i%4];

    if(opcode==WS_OP_PING){
        /* send pong */
        uint8_t pong[2]={0x80|WS_OP_PONG,0};
        send(cl->fd,pong,2,MSG_NOSIGNAL);
    } else if(opcode==WS_OP_CLOSE){
        close(cl->fd); cl->fd=-1; cl->state=STATE_HTTP;
    }
    cl->buf_len=0;
}

/* ─── Worker thread pool ────────────────────────────────── */
typedef struct { int fd; } WorkItem;

#define WORK_QUEUE_SIZE 256
static WorkItem  g_work_queue[WORK_QUEUE_SIZE];
static int       g_wq_head=0, g_wq_tail=0;
static pthread_mutex_t g_wq_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wq_cv=PTHREAD_COND_INITIALIZER;

static void wq_push(int fd){
    pthread_mutex_lock(&g_wq_mu);
    g_work_queue[g_wq_tail%WORK_QUEUE_SIZE].fd=fd;
    g_wq_tail++;
    pthread_cond_signal(&g_wq_cv);
    pthread_mutex_unlock(&g_wq_mu);
}

static int wq_pop(int *fd){
    pthread_mutex_lock(&g_wq_mu);
    while(g_wq_head==g_wq_tail && g_running)
        pthread_cond_wait(&g_wq_cv,&g_wq_mu);
    if(!g_running){pthread_mutex_unlock(&g_wq_mu);return 0;}
    *fd=g_work_queue[g_wq_head%WORK_QUEUE_SIZE].fd;
    g_wq_head++;
    pthread_mutex_unlock(&g_wq_mu);
    return 1;
}

static void* worker_thread(void *arg){
    (void)arg;
    int fd;
    while(wq_pop(&fd)){
        pthread_mutex_lock(&g_clients_mu);
        Client *cl=(fd>=0&&fd<MAX_CLIENTS)?&g_clients[fd]:NULL;
        if(!cl||cl->fd!=fd){pthread_mutex_unlock(&g_clients_mu);continue;}

        /* Read available data */
        int space=sizeof(cl->buf)-cl->buf_len-1;
        int n=(space>0)?recv(fd,cl->buf+cl->buf_len,space,0):0;
        if(n<=0){
            if(cl->fd>0){close(cl->fd);}
            cl->fd=-1;cl->state=STATE_HTTP;cl->buf_len=0;
            pthread_mutex_unlock(&g_clients_mu);
            continue;
        }
        cl->buf_len+=n;
        cl->buf[cl->buf_len]='\0';

        if(cl->state==STATE_HTTP){
            /* Wait for full HTTP request (ends with \r\n\r\n) */
            if(strstr(cl->buf,"\r\n\r\n"))
                handle_http(cl);
        } else {
            handle_ws_frame(cl);
        }
        pthread_mutex_unlock(&g_clients_mu);
    }
    return NULL;
}

/* ─── Accept loop (main thread via epoll) ───────────────── */
static void accept_client(void){
    struct sockaddr_in addr; socklen_t len=sizeof(addr);
    int cfd=accept(g_server_fd,(struct sockaddr*)&addr,&len);
    if(cfd<0)return;
    if(cfd>=MAX_CLIENTS){close(cfd);return;}
    set_nonblocking(cfd);

    pthread_mutex_lock(&g_clients_mu);
    g_clients[cfd].fd=cfd;
    g_clients[cfd].state=STATE_HTTP;
    g_clients[cfd].buf_len=0;
    pthread_mutex_unlock(&g_clients_mu);

    struct epoll_event ev={.events=EPOLLIN|EPOLLET,.data.fd=cfd};
    epoll_ctl(g_epoll_fd,EPOLL_CTL_ADD,cfd,&ev);
    printf("[tcp] accepted fd=%d from %s\n",cfd,inet_ntoa(addr.sin_addr));
}

/* ─── Signal handler ─────────────────────────────────────── */
static void on_signal(int s){(void)s;g_running=0;}

/* ─── main ───────────────────────────────────────────────── */
int main(void){
    srand((unsigned)time(NULL));
    signal(SIGINT,on_signal); signal(SIGTERM,on_signal);
    signal(SIGPIPE,SIG_IGN);

    /* Create listening socket */
    g_server_fd=socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(g_server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(PORT),.sin_addr.s_addr=INADDR_ANY};
    bind(g_server_fd,(struct sockaddr*)&addr,sizeof(addr));
    listen(g_server_fd,BACKLOG);
    set_nonblocking(g_server_fd);

    /* epoll */
    g_epoll_fd=epoll_create1(0);
    struct epoll_event ev={.events=EPOLLIN,.data.fd=g_server_fd};
    epoll_ctl(g_epoll_fd,EPOLL_CTL_ADD,g_server_fd,&ev);

    /* Initialize client table */
    memset(g_clients,0,sizeof(g_clients));
    for(int i=0;i<MAX_CLIENTS;i++) g_clients[i].fd=-1;

    /* Worker threads */
    pthread_t workers[WORKER_THREADS];
    for(int i=0;i<WORKER_THREADS;i++)
        pthread_create(&workers[i],NULL,worker_thread,NULL);

    /* Data generation thread */
    pthread_t dthr;
    pthread_create(&dthr,NULL,data_thread,NULL);

    printf("\n╔══════════════════════════════════════╗\n");
    printf("║  RT Chart Server  –  port %d        ║\n", PORT);
    printf("║  Workers: %d    Channels: %d           ║\n", WORKER_THREADS, DATA_CHANNELS);
    printf("║  Open → http://localhost:%d          ║\n", PORT);
    printf("╚══════════════════════════════════════╝\n\n");

    /* Event loop */
    struct epoll_event events[MAX_EVENTS];
    while(g_running){
        int n=epoll_wait(g_epoll_fd,events,MAX_EVENTS,200);
        for(int i=0;i<n;i++){
            int fd=events[i].data.fd;
            if(fd==g_server_fd){accept_client();}
            else{ wq_push(fd); }
        }
    }

    printf("\n[srv] shutting down…\n");
    pthread_cond_broadcast(&g_wq_cv);
    for(int i=0;i<WORKER_THREADS;i++) pthread_join(workers[i],NULL);
    pthread_join(dthr,NULL);
    close(g_server_fd); close(g_epoll_fd);
    return 0;
}
