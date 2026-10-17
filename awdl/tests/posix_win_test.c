/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "posix_win.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
static int failures;
static void check(int ok,const char *message){printf("%s %s\n",ok?"ok":"FAIL",message);if(!ok)++failures;}
int main(void){
    char *args[]={"awdld","-vv","-c6","-n","-s","1","--","-r"};
    winawdl_optind=0;
    check(winawdl_getopt(8,args,"c:nrs:lj:v")=='v',"first grouped option");
    check(winawdl_getopt(8,args,"c:nrs:lj:v")=='v',"second grouped option");
    check(winawdl_getopt(8,args,"c:nrs:lj:v")=='c' && !strcmp(winawdl_optarg,"6"),"attached channel");
    check(winawdl_getopt(8,args,"c:nrs:lj:v")=='n' && !winawdl_optarg,"no-host flag clears argument");
    check(winawdl_getopt(8,args,"c:nrs:lj:v")=='s' && !strcmp(winawdl_optarg,"1"),"separate argument");
    check(winawdl_getopt(8,args,"c:nrs:lj:v")==-1 && winawdl_optind==7,"double dash ends options");
    char *missing[]={"awdld","-c"};winawdl_optind=0;
    check(winawdl_getopt(2,missing,"c:")=='?' && !winawdl_optarg,"missing argument rejected");
    char *unknown[]={"awdld","-z"};winawdl_optind=0;
    check(winawdl_getopt(2,unknown,"c:")=='?',"unknown option rejected");
    struct timespec a,b;
    check(winawdl_clock_gettime(0,&a)==-1 && winawdl_clock_gettime(1,NULL)==-1,"invalid clock calls rejected");
    check(winawdl_clock_gettime(1,&a)==0,"monotonic clock available");Sleep(10);
    check(winawdl_clock_gettime(1,&b)==0 && (b.tv_sec>a.tv_sec || (b.tv_sec==a.tv_sec && b.tv_nsec>a.tv_nsec)),"monotonic clock advances");
    check(a.tv_nsec>=0 && a.tv_nsec<1000000000L && b.tv_nsec>=0 && b.tv_nsec<1000000000L,"normalized nanoseconds");
    return failures?1:0;
}
