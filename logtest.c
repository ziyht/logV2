#include "logtest.h"

void logTest()
{
    logShow("------- logShowAPI test ------\n");
    logShowTime();
    logShowText("[logShowText]\n");
    logShow("[logShow] This is a test logshow\n");

    logShow("------- logsysAPI test ------\n");
    logsysFlieEmpty();          // err, 服务未开
    logsysInit();               // 服务初始化 / 开启服务()
    logsysFlieEmpty();
    logsysSetMutetype(NMUTE);   // 修改日志系统服务为 非静默模式, 以下相关操作的信息都应该输出到控制台
    logsysSetFileSize(10);      // 这时, 应在控制台输出修改信息
    logsysStop();
    logsysRelease();            // 服务释放, 下面将无法创建

    logShow("------- logAPI 创建及设置测试 ------\n");
    logsysAdd(NULL, "创建测试:服务未开, 应输出错误提示\n");
    logCreate("test_mute_log", "./logs/test_mute_log.out", MUTE);       // 输出错误提示
    logCreate("test_nmute_log", "./logs/test_nmute_log.out", NMUTE);    // 输出错误提示

    logsysInit();   // 开启服务
    logsysAdd(NULL, "创建测试:服务已开, 应正常创建\n");
    logCreate("test_mute_log", "./logs/test_mute_log.out", MUTE);       // 正常创建
    logCreate("test_nmute_log", "./logs/test_nmute_log.out", NMUTE);    // 正常创建

    logsysAdd(NULL, "创建测试:不能创建已存在的日志(以名字区分)\n");
    logCreate("test_mute_log", "./logs/test_mute_log.out", MUTE);       // 错误, 已创建过 test_mute_log
    logCreate("test_nmute_log", "./logs/test_nmute_log.out", NMUTE);    // 错误, 已创建过 test_nmute_log

    logsysAdd(NULL, "删除测试:已存在的日志正常删除\n");
    logDestroy("test_mute_log");

    logsysAdd(NULL, "删除测试:不存在的日志删除异常\n");
    logDestroy("test_mute_log");

    logsysAdd(NULL, "设置测试:已存在的日志设置正常\n");
    logShow("%u \n", logFileSize("test_nmute_log"));
    logSetFileSize("test_nmute_log", 100);
    logSetMutetype("test_nmute_log", MUTE);
    logFlieEmpty("test_nmute_log");

    logsysAdd(NULL, "设置测试:不存在的日志设置异常\n");
    logShow("%u \n", logFileSize("test_mute_log"));
    logSetFileSize("test_mute_log", 100);
    logSetMutetype("test_mute_log", MUTE);
    logFlieEmpty("test_mute_log");

    logsysShow("日志测试: 正常添加测试\n");
    logsysSetMutetype(MUTE);   // 修改日志系统服务为 静默模式
    logCreate("test_mute_log", "./logs/test_mute_log.out", MUTE);   // 重新创建 test_mute_log, 静默添加
    logSetMutetype("test_nmute_log", NMUTE);                        // 还原 test_nmute_log 为非静默模式

    logAdd("test_mute_log", "a [mute] test log, not to console\n");                           // 控制台不输出
    logAddTime("test_mute_log");                                                              // 控制台不输出
    logAddText("test_mute_log", "a [mute] test log, not to console\n");                       // 控制台不输出

    logAddMute("test_mute_log", "a [mute] test log in mute mode, not to console\n");          // 控制台不输出
    logAddTimeMute("test_mute_log");                                                          // 控制台不输出
    logAddTextMute("test_mute_log", "a [mute] test log in mute mode, not to console\n");      // 控制台不输出

    logAddNMute("test_mute_log", "a [mute] test log in nmute mode, should to console\n");     // 控制台  输出
    logAddTimeNMute("test_mute_log");                                                         // 控制台  输出
    logAddTextNMute("test_mute_log", "a [mute] test log in nmute mode, should to console\n"); // 控制台  输出

    logAdd("test_nmute_log", "a [nmute] test log, should to console\n");                      // 控制台  输出
    logAddTime("test_nmute_log");                                                             // 控制台  输出
    logAddText("test_nmute_log", "a [nmute] test log, should to console\n");                  // 控制台  输出

    logAddMute("test_nmute_log", "a [nmute] test log in mute mode, not to console\n");         // 控制台不输出
    logAddTimeMute("test_nmute_log");                                                          // 控制台不输出
    logAddTextMute("test_nmute_log", "a [nmute] test log in mute mode, not to console\n");     // 控制台不输出

    logAddNMute("test_nmute_log", "a [nmute] test log in nmute mode, should to console\n");     // 控制台  输出
    logAddTimeNMute("test_nmute_log");                                                          // 控制台  输出
    logAddTextNMute("test_nmute_log", "a [nmute] test log in nmute mode, should to console\n"); // 控制台  输出


    logsysShow("日志测试: 异常日志信息添加测试, 不作任何处理\n");
    logAdd("test_nmute_log", "");
    logAddText("test_nmute_log", NULL);

    logAddMute("test_nmute_log", "");
    logAddTextMute("test_nmute_log", NULL);

    logAddNMute("test_nmute_log", "");
    logAddTextNMute("test_nmute_log", NULL);

    logsysShow("日志测试: 添加信息到不存在的日志中, 输出提示信息\n");
    logAdd("000", "a [nmute] test log, should to console\n");                      // 控制台  输出
    logAddTime("000");                                                             // 控制台  输出
    logAddText("000", "a [nmute] test log, should to console\n");                  // 控制台  输出

    logAddMute("000", "a [nmute] test log in mute mode, not to console\n");         // 控制台  输出
    logAddTimeMute("000");                                                          // 控制台  输出
    logAddTextMute("000", "a [nmute] test log in mute mode, not to console\n");     // 控制台  输出

    logAddNMute("000", "a [nmute] test log in nmute mode, should to console\n");     // 控制台  输出
    logAddTimeNMute("000");                                                          // 控制台  输出
    logAddTextNMute("000", "a [nmute] test log in nmute mode, should to console\n"); // 控制台  输出

    logsysShow("路径测试: 无权限测试\n");
    logCreate("path_test", "/root/path_test.out", MUTE);

    logsysRelease();            //
}

/* 使用示例 */
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
