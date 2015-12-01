## logV2

###简单日志系统 v2.0
此模块用于简单输出日志到屏幕和写入到文件
系统内部使用 redisdic 维护 日志结构数据, 使用 日志结构的 name 进行区分, 用户不用关心底层,
所以即使是多线程, 也可以通过 name 把信息添加到指定的日志结构中

使用的 redisdic 结构是 redis3.0 中的原型, 但是所有的函数都重新命名, 一般情况下不会和正常的 redis 冲突

###使用方式:
  1. logsysInit()    开启日志系统, 如有必要, 可使用 logsysSet*() 相关函数进行相关设置
  2. logCreate()     添加用户日志结构
  3. logAdd*()       添加日志到指定的日志结构中, 每次添加会保证把信息写入到关联的文件中
  4. logsysRelease() 停止和释放资源, 其实这一步可省略, 日志系统应该时伴随整个程序流程的, 程序结束的时候会自动释放资源
                     在使用 valgrind 测试时一定要加上这一步

###代码示例：
```c
void normalTest()
{
    logsysShow("常规正常测试: 以下不应输出任何正常提示\n");

    logsysRelease();
    logsysInit();

    logCreate("log1", "./logs/log1.out", MUTE);
    logCreate("log2", "./logs/log2.out", MUTE);

    int i = 0;
    for(; i < 10; i++)
    {
        logAdd("log1", "Hello log1\n");
        sleep(1);
        logAdd("log2", "Hello log2\n");
        sleep(1);
    }

    logsysRelease();
}
```

###相关说明:
  0. 系统日志文件在  ./logs/sys.out 中, 不可通过 API 修改, 如要更改, 修改宏定义 LOGSYS_PATH 即可
  1. 系统日志大小默认为 1M, 即当系统日志文件大于 1M 时, 会自动清空, 可通过 logsysSetFileSize(size_mb), 进行设置, 但每次都须重新设置
  2. 系统日志默认为静默模式, 即所有的正常操作只记录到日志中, 不输出到控制台, 但操作异常会输出相关信息到控制台
  3. 用户日志大小默认为 100M, 可使用 logSetFileSize(size_mb), 每次使用都须重新设置, 每个用户日志均有自己的属性, 互不影响

###注意:
  本日志系统并没有执行相同文件测试, 即两个日志结构可以指向同一个文件
