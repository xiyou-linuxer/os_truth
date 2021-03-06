#include "init.h"
#include "print.h"
#include "interrupt.h"
#include "../device/timer.h"
#include "memory.h"
#include "thread.h"
#include "console.h"
#include "keyboard.h"
#include "tss.h"
#include "syscall-init.h"
#include "ide.h"
#include "fs.h"

/*负责初始化所有模块*/
void init_all()
{
    put_str("init_all\n");
    idt_init();   //初始化中断
    mem_init();   //初始化内存池，内存管理系统
    thread_init();   //线程初始化
    timer_init(); //初始化PIT
    console_init();   //控制台初始化
    keyboard_init();   //键盘初始化
    tss_init();   //tss初始化
    syscall_init();   //系统调用初始化
    intr_enable();   //后面的需要开中断
    ide_init();   //分区初始化
    filesys_init();   //文件系统初始化
}
