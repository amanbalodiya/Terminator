/* Task 6: multiWatch ["command", "command", ...].
 * A separate process group per command; poll() multiplexes all output.
 */
#include "shell.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MW_MAX 32
static volatile sig_atomic_t mw_stop=0;
static void mw_on_int(int sig){(void)sig;mw_stop=1;}
#define MW_INTERVAL 2
#define MW_SEPARATOR "----------------------------------------------------\n"
typedef struct { char *cmd; pid_t pid; int fd; time_t next; char *buf; size_t len, cap; } mw_item;

static void trim(char *s) { size_t n = strlen(s); while(n && isspace((unsigned char)s[n-1])) s[--n]=0; }
/* Parse the original statement, preserving spaces and quotes inside commands. */
static int parse_list(shell_t *s, const char *src, mw_item *it) {
    const char *p = src;
    while (isspace((unsigned char)*p)) p++;
    if (strncmp(p,"multiWatch",10) || (p[10] && !isspace((unsigned char)p[10]) && p[10]!='[')) return -1;
    p += 10; while(isspace((unsigned char)*p)) p++;
    if (*p++!='[') return -1;
    int n=0;
    for (;;) {
        while(isspace((unsigned char)*p)) p++;
        if (*p==']') { p++; break; }
        if (n==MW_MAX || (*p!='"' && *p!='\'')) return -1;
        char quote=*p++; size_t cap=128, len=0; char *str=malloc(cap);
        if(!str) return -1;
        while(*p && *p!=quote) {
            char c=*p++;
            if(c=='\\' && (*p==quote || *p=='\\')) c=*p++;
            if(len+2>cap) { cap*=2; char *tmp=realloc(str,cap); if(!tmp){free(str);return -1;} str=tmp; }
            str[len++]=c;
        }
        if(*p!=quote) {free(str);return -1;}
        p++; str[len]=0; trim(str);
        if(!len) {free(str);return -1;}
        it[n++].cmd=str;
        while(isspace((unsigned char)*p)) p++;
        if(*p==',') {p++;continue;}
        if(*p==']') {p++;break;}
        return -1;
    }
    while(isspace((unsigned char)*p)) p++;
    if(*p || !n) return -1;
    (void)s; return n;
}
static void emit(shell_t *s, mw_item *it, time_t timestamp) {
    sh_out(s,"\"%s\", %ld:\n%s",it->cmd,(long)timestamp,MW_SEPARATOR);
    if(it->len) { write_all(s->out_fd,it->buf,it->len); if(it->buf[it->len-1]!='\n') sh_out(s,"\n"); }
    sh_out(s,"%s",MW_SEPARATOR);
    it->len=0;
}
static int launch(shell_t *s,mw_item *it) {
    int pp[2]; if(pipe(pp)<0) return -1;
    pid_t pid=fork();
    if(pid<0){close(pp[0]);close(pp[1]);return -1;}
    if(pid==0) {
        setpgid(0,0);
        close(pp[0]);
        dup2(pp[1],STDOUT_FILENO); dup2(pp[1],STDERR_FILENO); close(pp[1]);
        if(s->in_fd>=0) dup2(s->in_fd,STDIN_FILENO);
        else {int nullfd=open("/dev/null",O_RDONLY);if(nullfd>=0){dup2(nullfd,STDIN_FILENO);close(nullfd);}}
        if(chdir(s->cwd)<0) _exit(126);
        execl("/bin/sh","sh","-c",it->cmd,(char*)NULL);
        _exit(127);
    }
    setpgid(pid,pid); close(pp[1]);
    fcntl(pp[0],F_SETFL,fcntl(pp[0],F_GETFL,0)|O_NONBLOCK);
    it->fd=pp[0]; it->pid=pid; it->next=0; it->len=0;
    return 0;
}
static void append(mw_item *it,const char *buf,size_t n) {
    if(it->len+n+1>it->cap) {
        size_t c=it->cap?it->cap:1024;
        while(c<it->len+n+1) c*=2;
        char *p=realloc(it->buf,c); if(!p) return;
        it->buf=p;it->cap=c;
    }
    memcpy(it->buf+it->len,buf,n);it->len+=n;it->buf[it->len]=0;
}
int multiwatch_run(shell_t *s,int argc,char **argv) {
    (void)argc;(void)argv;
    mw_item it[MW_MAX]={0}; int n=parse_list(s,s->cur_stmt?s->cur_stmt:"",it);
    if(n<0) {sh_err(s,"usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");for(int i=0;i<MW_MAX;i++)free(it[i].cmd);return 2;}
    for(int i=0;i<n;i++)it[i].fd=-1;
    pid_t controller=fork();
    if(controller<0){sh_err(s,"multiWatch: fork: %s\n",strerror(errno));goto fail;}
    if(controller==0) {
        /* Controller handles GUI's SIGINT and forwards it to every command group. */
        setpgid(0,0);
        struct sigaction sa={0};sa.sa_handler=mw_on_int;sigaction(SIGINT,&sa,NULL);
        int active=1;
        for(int i=0;i<n;i++) if(launch(s,&it[i])<0) active=0;
        while(active && !mw_stop) {
            struct pollfd fds[MW_MAX];int indices[MW_MAX],count=0;
            time_t now=time(NULL);
            for(int i=0;i<n;i++) {
                if(it[i].fd>=0){fds[count]=(struct pollfd){.fd=it[i].fd,.events=POLLIN|POLLHUP|POLLERR};indices[count++]=i;}
                else if(it[i].pid==0 && now>=it[i].next) launch(s,&it[i]);
            }
            int r=poll(fds,count,200);
            if(r<0 && errno!=EINTR)break;
            for(int j=0;j<count;j++) if(fds[j].revents) {
                int i=indices[j];char buf[4096];ssize_t k;
                while((k=read(it[i].fd,buf,sizeof buf))>0) append(&it[i],buf,(size_t)k);
                if(k==0 || (k<0 && errno!=EAGAIN && errno!=EWOULDBLOCK)) {
                    close(it[i].fd);it[i].fd=-1;
                    int st;while(waitpid(it[i].pid,&st,0)<0 && errno==EINTR){}
                    emit(s,&it[i],time(NULL));it[i].pid=0;it[i].next=time(NULL)+MW_INTERVAL;
                }
            }
        }
        for(int i=0;i<n;i++) {if(it[i].pid>0)kill(-it[i].pid,SIGTERM);if(it[i].fd>=0)close(it[i].fd);}
        for(int i=0;i<n;i++) if(it[i].pid>0) waitpid(it[i].pid,NULL,0);
        _exit(0);
    }
    setpgid(controller,controller);
    s->fg_pgid=controller;
    int status=0;
    while(waitpid(controller,&status,0)<0 && errno==EINTR){}
    s->fg_pgid=0;
    for(int i=0;i<n;i++){free(it[i].cmd);free(it[i].buf);}
    return WIFSIGNALED(status)?128+WTERMSIG(status):WIFEXITED(status)?WEXITSTATUS(status):1;
fail:
    for(int i=0;i<n;i++){free(it[i].cmd);free(it[i].buf);}
    return 1;
}
