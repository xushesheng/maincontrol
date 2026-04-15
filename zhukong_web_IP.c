#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <time.h>
/*▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
  ▓                          程序初始参量配置信息                                 ▓
  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓*/
#define MEMORY                 0x00000000                   //定义共享内存基地址
#define SELF_PORT              3408                         //定义本机网络通讯端口号
#define WEB_ADDR               "192.168.1.12"               //定义网管程序地址配置
#define WEB_PORT               3409                         //定义网管程序端口号
#define BUF_SIZE               2048                         //定义网络接收数据长度
#define LINK_BAD			   0				            //定义建链失败
#define LINK_OK				   1				            //定义建链成功
#define SEND_EN				   1				            //定义允许发送使能
#define SEND_OFF			   0				            //定义允许发送禁止
#define SAVA_DATA_LENG        221                           //定义存储信息数据的最大长度字节数
/*※※※※※※※※※※※※※※※※※※※结构体全局变量信息※※※※※※※※※※※※※※※※※※※※*/
/*※※※※※※※※※※※※※※※※※※※主控-->web信息※※※※※※※※※※※※※※※※※※※※*/
struct speechdata							                //定义网管和机箱管理收、发建链数据帧格式
{
	unsigned short frameHead;					            //帧头  ==0xF00F
	unsigned char  frameCnt;				                //帧序列号  ==0x00
	unsigned char  linkMark;					            //建链标识  ==0xaa
	unsigned short frameEnd;					            //帧尾  ==0x0EE0
} TxNetSpeechData,RxNetSpeechData;
struct positiveAck              /*定义主控返回web网管肯定应答数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0D00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==03
  unsigned char frameCnt;                                   //帧计数 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和) ==03
}positive_ack;
struct negativeAck              /*定义主控返回web网管否定应答数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0D00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==04
  unsigned char frameCnt;                                   //帧计数 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和) ==04
}negative_ack;
struct backjobmodeQuery         /*定义主控返回web网管工作参数查询结果数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X4A00  64+10
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==05
  unsigned char frameCnt;                                   //帧计数 ==37
  unsigned char TPDPoptioncode;                             //跳频定频操作码 ==C1
  unsigned char TPDPdatecode;                               //跳频定频数据码
  unsigned char ZWFSoptioncode;                             //组网方式操作码 ==C2
  unsigned char ZWFSdatecode;                               //组网方式数据码
  unsigned char QOSoptioncode;                              //QOS等级操作码  ==C3
  unsigned char QOSdatecode;                                //QOS等级数据码
  unsigned char TPMSoptioncode;                             //跳频模式操作码 ==C4
  unsigned char TPMSdatecode;                               //跳频模式数据码
  unsigned char TPSLoptioncode;                             //跳频速率操作码 ==C5
  unsigned char TPSLdatecode;                               //跳频速率数据码 ==00
  unsigned char TPPBoptioncode;                             //跳频频表操作码 ==C6
  unsigned char TPPBdatecode;                               //跳频频表数据码 ==00
  unsigned char TPXLoptoncode;                              //跳频序列操作码 ==C7
  unsigned char TPXLdatecode;                               //跳频序列数据码 ==00
  unsigned char TZDKoptioncode;                             //调制带宽操作码 ==C8
  unsigned char TZDKdatecode;                               //调制带宽数据码 ==00
  unsigned char GLSJoptioncode;                             //功率衰减操作码 ==C9
  unsigned char GLSJdatecode;                               //功率衰减数据码
  unsigned char JSTDZToptioncode;                           //接受通道状态操作码 ==CA
  unsigned char JSTDZTdatecode;                             //接受通道状态数据码
  unsigned char FSTDZToptioncode;                           //发射通道状态操作码 ==CB
  unsigned char FSTDZTdatecode;                             //发射通道状态数据码
  unsigned char TXLXoptioncode;                             //天线类型操作码 ==CC
  unsigned char TXLXdatecode;                               //天线类型数据码
  unsigned char PL1optioncode;                              //频率1000M和100MHz操作码 ==CD
  unsigned char PL1datecode;                                //频率1000M和100MHz数据码
  unsigned char PL2optioncode;                              //频率10M和1MHz操作码 ==CE
  unsigned char PL2datecode;                                //频率10M和1MHz数据码
  unsigned char PL3optioncode;                              //频率100K和10KHz操作码 ==CF
  unsigned char PL3datecode;                                //频率100K和10KHz数据码
  unsigned char PL4optioncode;                              //频率1K和100Hz操作码 ==D0
  unsigned char PL4datecode;                                //频率1K和100Hz数据码
  unsigned char PL5optioncode;                              //频率10和1Hz操作码 ==D1
  unsigned char PL5datecode;                                //频率10和1Hz数据码
  unsigned char IPB1optioncode;                             //IP地址B1操作码 ==D2
  unsigned char IPB1datecode;                               //IP地址B1数据码
  unsigned char IPB2optioncode;                             //IP地址B2操作码 ==D3
  unsigned char IPB2datecode;                               //IP地址B2数据码
  unsigned char IPB3optioncode;                             //IP地址B3操作码 ==D4
  unsigned char IPB3datecode;                               //IP地址B3数据码
  unsigned char IPB4optioncode;                             //IP地址B4操作码 ==D5
  unsigned char IPB4datecode;                               //IP地址B4数据码
  unsigned char IPYMB1optioncode;                           //IP地址掩码B1操作码 ==D6
  unsigned char IPYMB1datecode;                             //IP地址掩码B1数据码
  unsigned char IPYMB2optioncode;                           //IP地址掩码B2操作码 ==D7
  unsigned char IPYMB2datecode;                             //IP地址掩码B2数据码
  unsigned char IPYMB3optioncode;                           //IP地址掩码B3操作码 ==D8
  unsigned char IPYMB3datecode;                             //IP地址掩码B3数据码
  unsigned char IPYMB4optioncode;                           //IP地址掩码B4操作码 ==D9
  unsigned char IPYMB4datecode;                             //IP地址掩码B4数据码
  unsigned char WGDZB1optioncode;                           //网关地址B1操作码 ==DA
  unsigned char WGDZB1datecode;                             //网关地址B1数据码
  unsigned char WGDZB2optioncode;                           //网关地址B2操作码 ==DB
  unsigned char WGDZB2datecode;                             //网关地址B2数据码
  unsigned char WGDZB3optioncode;                           //网关地址B3操作码 ==DC
  unsigned char WGDZB3datecode;                             //网关地址B3数据码
  unsigned char WGDZB4optioncode;                           //网关地址B4操作码 ==DD
  unsigned char WGDZB4datecode;                             //网关地址B4数据码
  unsigned char TZFSoptioncode;                             //调制方式操作码 ==DE
  unsigned char TZFSdatecode;                               //调制方式数据码
  unsigned char XDBMotioncode;                              //信道编码操作码 ==DF
  unsigned char XDBMdatecode;                               //信道编码数据码
}back_jobmode_set;
struct selftestReport           /*定义主控返回web网管自检查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0E     00001110
  unsigned char frameCnt;                                   //帧计数 ==02     00000010
  unsigned char selftestoptioncode;                         //自检操作码 ==e1 11100001
  unsigned char selftestdatecode;                           //自检数据码 ==04
  unsigned char frameEnd;                                   //帧尾(即校验和) ==E9
}selftest_report;
struct versionReport            /*定义主控返回web网管版本查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1300
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==10          00010000
  unsigned char frameCnt;                                   //帧计数 ==06          00001111
  unsigned char PLVoptioncode;                              //PL版本操作码 ==E1    11100001
  unsigned char PLVdatecode;                                //PL版本数据码
  unsigned char MACVoptioncode;                             //MAC版本操作码 ==E2   11100010
  unsigned char MACVdatecode;                               //MAC版本数据码
  unsigned char NETVoptioncode;                             //NET版本操作码 ==E3   11100011
  unsigned char NETVdatecode;                               //NET版本数据码
  unsigned char frameEnd;                                   //帧尾(即校验和) ==FF  11111111
}version_report;


/*※※※※※※※※※※※※※※※※※※※web-->主控信息※※※※※※※※※※※※※※※※※※※※*/
struct jobmodeQuery             /*定义接收web网管工作参数查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==01
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char queryOptioncode;                            //查询请求操作码
  unsigned char queryDatecode;                              //查询请求数据码
}jobmode_query;
struct jobmodeSet               /*定义接收web网管工作模式配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1300
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==01
  unsigned char frameCnt;                                   //帧计数 ==08
  unsigned char TPDPoptioncode;                             //跳频定频操作码 ==C1
  unsigned char TPDPdatecode;                               //跳频定频数据码
  unsigned char ZWFSoptioncode;                             //组网方式操作码 ==C2
  unsigned char ZWFSdatecode;                               //组网方式数据码
  unsigned char QOSoptioncode;                              //QOS等级操作码 ==C3
  unsigned char QOSdatecode;                                //QOS等级数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}jobmode_set;
struct TPmodeSet                /*定义接收web网管跳频模式配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==05
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TPmodeoptioncode;                           //跳频模式操作码 ==C4
  unsigned char TPmodedatecode;                             //跳频模式数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TPmode_set;
struct DPmodeSet                /*定义接收web网管定频配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1700
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==06
  unsigned char frameCnt;                                   //帧计数 ==0A
  unsigned char DPPL1optioncode;                            //频率1000M和100MHz操作码 ==CD
  unsigned char DPPL1datecode;                              //频率1000M和100MHz数据码
  unsigned char DPPL2optioncode;                            //频率10M和1MHz操作码 ==CE
  unsigned char DPPL2datecode;                              //频率10M和1MHz数据码
  unsigned char DPPL3optioncode;                            //频率100K和10KHz操作码 ==CF
  unsigned char DPPL3datecode;                              //频率100K和10KHz数据码
  unsigned char DPPL4optioncode;                            //频率1K和100Hz操作码 ==D0
  unsigned char DPPL4datecode;                              //频率1K和100Hz数据码
  unsigned char DPPL5optioncode;                            //频率10和1Hz操作码 ==D1
  unsigned char DPPL5datecode;                              //频率10和1Hz数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}DPmode_set;
struct TPCSSet                  /*定义接收web网管跳频参数预置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1300
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==07
  unsigned char frameCnt;                                   //帧计数 ==06
  unsigned char TPSLoptioncode;                             //跳频速率操作码 ==C5
  unsigned char TPSLdatecode;                               //跳频速率数据码
  unsigned char TPPBoptioncode;                             //跳频频表操作码 ==C6
  unsigned char TPPBdatecode;                               //跳频频表数据码
  unsigned char TPXLoptioncode;                             //跳频序列操作码 ==C7
  unsigned char TPXLdatecode;                               //跳频序列数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TPCS_set;
struct TZDKSet                 /*定义接收web网管调制带宽配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==08
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TZKDoptioncode;                             //调制带宽操作码 ==C8
  unsigned char TZKDdatecode;                               //调制带宽数据码 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TZDK_set;
struct TZFSSet                 /*定义接收web网管调制方式配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==09
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TZFSoptioncode;                             //调制方式操作码 ==DE
  unsigned char TZFSdatecode;                               //调制方式数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TZFS_set;
struct XDBMSet                 /*定义接收web网管信道编码配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0A
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char XDBMoptioncode;                             //信道编码操作码 ==DF
  unsigned char XDBMdatecode;                               //信道编码数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}XDBM_set;
struct GLSJSet                 /*定义接收web网管信道功率衰减配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0B
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char GLSJoptioncode;                             //信道编码操作码 ==C9
  unsigned char GLSJdatecode;                               //信道编码数据码 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和)
}GLSJ_set;
struct WLCSSet                 /*定义接收web网管网络参数配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X2500
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0C
  unsigned char frameCnt;                                   //帧计数 ==18
  unsigned char IPB1optioncode;                             //IP地址B1操作码 ==D2
  unsigned char IPB1datecode;                               //IP地址B1数据码
  unsigned char IPB2optioncode;                             //IP地址B2操作码 ==D3
  unsigned char IPB2datecode;                               //IP地址B2数据码
  unsigned char IPB3optioncode;                             //IP地址B3操作码 ==D4
  unsigned char IPB3datecode;                               //IP地址B3数据码
  unsigned char IPB4optioncode;                             //IP地址B4操作码 ==D5
  unsigned char IPB4datecode;                               //IP地址B4数据码
  unsigned char IPYMB1optioncode;                           //IP地址掩码B1操作码 ==D6
  unsigned char IPYMB1datecode;                             //IP地址掩码B1数据码
  unsigned char IPYMB2optioncode;                           //IP地址掩码B2操作码 ==D7
  unsigned char IPYMB2datecode;                             //IP地址掩码B2数据码
  unsigned char IPYMB3optioncode;                           //IP地址掩码B3操作码 ==D8
  unsigned char IPYMB3datecode;                             //IP地址掩码B3数据码
  unsigned char IPYMB4optioncode;                           //IP地址掩码B4操作码 ==D9
  unsigned char IPYMB4datecode;                             //IP地址掩码B4数据码
  unsigned char WGDZB1optioncode;                           //网关地址B1操作码 ==DA
  unsigned char WGDZB1datecode;                             //网关地址B1数据码
  unsigned char WGDZB2optioncode;                           //网关地址B2操作码 ==DB
  unsigned char WGDZB2datecode;                             //网关地址B2数据码
  unsigned char WGDZB3optioncode;                           //网关地址B3操作码 ==DC
  unsigned char WGDZB3datecode;                             //网关地址B3数据码
  unsigned char WGDZB4optioncode;                           //网关地址B4操作码 ==DD
  unsigned char WGDZB4datecode;                             //网关地址B4数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}WLCS_set;
struct TXLXSet                 /*定义接收web网管天线类型设置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==11
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TXLXoptioncode;                             //天线类型操作码 ==CC
  unsigned char TXLXdatecode;                               //天线类型数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TXLX_set;
struct selftestQuery           /*定义接收web网管自检查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0E00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0D
  unsigned char frameCnt;                                   //帧计数 ==01
  unsigned char selftestqueryoptioncode;                    //自检查询操作码 ==e1
  unsigned char frameEnd;                                   //帧尾(即校验和)
}selftest_query;
struct versionQuery            /*定义接收web网管版本查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0E00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0F
  unsigned char frameCnt;                                   //帧计数 ==01
  unsigned char versionqueryoptioncode;                     //版本查询操作码 ==e5
  unsigned char frameEnd;                                   //帧尾(即校验和)
}version_query;
struct GZPBHFSet               /*定义接收web网管故障屏蔽与恢复设置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==12
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char GZoptioncode;                               //故障屏蔽与恢复操作码 ==E4
  unsigned char GZdatecode;                                 //故障屏蔽与恢复数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}GZPBHF_set;


/*※※※※※※※※※※※※※※※※※※※写入共享内存信息※※※※※※※※※※※※※※※※※※※※*/
struct jobmodeQuery             /*定义主控转发接收自web的工作参数查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==01
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char queryOptioncode;                            //查询请求操作码
  unsigned char queryDatecode;                              //查询请求数据码
}jobmode_query;
struct jobmodeSet              /*定义主控转发接收自web的工作模式配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1300
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==01
  unsigned char frameCnt;                                   //帧计数 ==08
  unsigned char TPDPoptioncode;                             //跳频定频操作码 ==C1
  unsigned char TPDPdatecode;                               //跳频定频数据码
  unsigned char ZWFSoptioncode;                             //组网方式操作码 ==C2
  unsigned char ZWFSdatecode;                               //组网方式数据码
  unsigned char QOSoptioncode;                              //QOS等级操作码 ==C3
  unsigned char QOSdatecode;                                //QOS等级数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}jobmode_set;
struct TPmodeSet                /*定义主控转发接收自web的跳频模式配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==05
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TPmodeoptioncode;                           //跳频模式操作码 ==C4
  unsigned char TPmodedatecode;                             //跳频模式数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TPmode_set;
struct DPmodeSet                /*定义主控转发接收自web的定频配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1700
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==06
  unsigned char frameCnt;                                   //帧计数 ==0A
  unsigned char DPPL1optioncode;                            //频率1000M和100MHz操作码 ==CD
  unsigned char DPPL1datecode;                              //频率1000M和100MHz数据码
  unsigned char DPPL2optioncode;                            //频率10M和1MHz操作码 ==CE
  unsigned char DPPL2datecode;                              //频率10M和1MHz数据码
  unsigned char DPPL3optioncode;                            //频率100K和10KHz操作码 ==CF
  unsigned char DPPL3datecode;                              //频率100K和10KHz数据码
  unsigned char DPPL4optioncode;                            //频率1K和100Hz操作码 ==D0
  unsigned char DPPL4datecode;                              //频率1K和100Hz数据码
  unsigned char DPPL5optioncode;                            //频率10和1Hz操作码 ==D1
  unsigned char DPPL5datecode;                              //频率10和1Hz数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}DPmode_set;
struct TPCSSet                  /*定义主控转发接收自web的跳频参数预置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1300
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==07
  unsigned char frameCnt;                                   //帧计数 ==06
  unsigned char TPSLoptioncode;                             //跳频速率操作码 ==C5
  unsigned char TPSLdatecode;                               //跳频速率数据码
  unsigned char TPPBoptioncode;                             //跳频频表操作码 ==C6
  unsigned char TPPBdatecode;                               //跳频频表数据码
  unsigned char TPXLoptioncode;                             //跳频序列操作码 ==C7
  unsigned char TPXLdatecode;                               //跳频序列数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TPCS_set;
struct TZDKSet                 /*定义主控转发接收自web的调制带宽配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==08
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TZKDoptioncode;                             //调制带宽操作码 ==C8
  unsigned char TZKDdatecode;                               //调制带宽数据码 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TZDK_set;
struct TZFSSet                 /*定义主控转发接收自web的调制方式配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==09
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TZFSoptioncode;                             //调制方式操作码 ==DE
  unsigned char TZFSdatecode;                               //调制方式数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TZFS_set;
struct XDBMSet                 /*定义主控转发接收自web的信道编码配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0A
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char XDBMoptioncode;                             //信道编码操作码 ==DF
  unsigned char XDBMdatecode;                               //信道编码数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}XDBM_set;
struct GLSJSet                 /*定义主控转发接收自web的信道功率衰减配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0B
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char GLSJoptioncode;                             //信道编码操作码 ==C9
  unsigned char GLSJdatecode;                               //信道编码数据码 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和)
}GLSJ_set;
struct WLCSSet                 /*定义主控转发接收自web的网络参数配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X2500
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0C
  unsigned char frameCnt;                                   //帧计数 ==18
  unsigned char IPB1optioncode;                             //IP地址B1操作码 ==D2
  unsigned char IPB1datecode;                               //IP地址B1数据码
  unsigned char IPB2optioncode;                             //IP地址B2操作码 ==D3
  unsigned char IPB2datecode;                               //IP地址B2数据码
  unsigned char IPB3optioncode;                             //IP地址B3操作码 ==D4
  unsigned char IPB3datecode;                               //IP地址B3数据码
  unsigned char IPB4optioncode;                             //IP地址B4操作码 ==D5
  unsigned char IPB4datecode;                               //IP地址B4数据码
  unsigned char IPYMB1optioncode;                           //IP地址掩码B1操作码 ==D6
  unsigned char IPYMB1datecode;                             //IP地址掩码B1数据码
  unsigned char IPYMB2optioncode;                           //IP地址掩码B2操作码 ==D7
  unsigned char IPYMB2datecode;                             //IP地址掩码B2数据码
  unsigned char IPYMB3optioncode;                           //IP地址掩码B3操作码 ==D8
  unsigned char IPYMB3datecode;                             //IP地址掩码B3数据码
  unsigned char IPYMB4optioncode;                           //IP地址掩码B4操作码 ==D9
  unsigned char IPYMB4datecode;                             //IP地址掩码B4数据码
  unsigned char WGDZB1optioncode;                           //网关地址B1操作码 ==DA
  unsigned char WGDZB1datecode;                             //网关地址B1数据码
  unsigned char WGDZB2optioncode;                           //网关地址B2操作码 ==DB
  unsigned char WGDZB2datecode;                             //网关地址B2数据码
  unsigned char WGDZB3optioncode;                           //网关地址B3操作码 ==DC
  unsigned char WGDZB3datecode;                             //网关地址B3数据码
  unsigned char WGDZB4optioncode;                           //网关地址B4操作码 ==DD
  unsigned char WGDZB4datecode;                             //网关地址B4数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}WLCS_set;
struct TXLXSet                 /*定义主控转发接收自web的天线类型设置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==11
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char TXLXoptioncode;                             //天线类型操作码 ==CC
  unsigned char TXLXdatecode;                               //天线类型数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}TXLX_set;
struct selftestQuery           /*定义主控转发接收自web的自检查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0E00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0D
  unsigned char frameCnt;                                   //帧计数 ==01
  unsigned char selftestqueryoptioncode;                    //自检查询操作码 ==e1
  unsigned char frameEnd;                                   //帧尾(即校验和)
}selftest_query;
struct versionQuery           /*定义主控转发接收自web的版本查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0E00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0F
  unsigned char frameCnt;                                   //帧计数 ==01
  unsigned char versionqueryoptioncode;                     //版本查询操作码 ==e5
  unsigned char frameEnd;                                   //帧尾(即校验和)
}version_query;
struct GZPBHFSet              /*定义主控转发接收自web的故障屏蔽与恢复设置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0F00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0D00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==12
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char GZoptioncode;                               //故障屏蔽与恢复操作码 ==E4
  unsigned char GZdatecode;                                 //故障屏蔽与恢复数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}GZPBHF_set;


/*※※※※※※※※※※※※※※※※※※※读取共享内存信息※※※※※※※※※※※※※※※※※※※※*/
struct positiveAck              /*定义组网软件返回主控肯定应答数据帧格式*/
{
  unsigned short frameHead;                         //帧头 ==0X0D00
  unsigned short frameRetain;                       //帧保留 ==0000
  unsigned short GoalId;                            //目的ID ==0B00
  unsigned short SourceID;                          //源ID ==0D00
  unsigned short synchronizing;                     //同步序列 ==FFF5
  unsigned char frameType;                          //帧类型 ==03
  unsigned char frameCnt;                           //帧计数 ==00
  unsigned char frameEnd;                           //帧尾(即校验和) ==03
}positive_ack;
struct negativeAck              /*定义组网软件返回主控否定应答数据帧格式*/
{
  unsigned short frameHead;                         //帧头 ==0X0D00
  unsigned short frameRetain;                       //帧保留 ==0000
  unsigned short GoalId;                            //目的ID ==0B00
  unsigned short SourceID;                          //源ID ==0D00
  unsigned short synchronizing;                     //同步序列 ==FFF5
  unsigned char frameType;                          //帧类型 ==04
  unsigned char frameCnt;                           //帧计数 ==00
  unsigned char frameEnd;                           //帧尾(即校验和) ==04
}negative_ack;
struct backjobmodeQuery         /*定义组网软件返回主控工作参数查询结果数据帧格式*/
{
  unsigned short frameHead;                         //帧头 ==0X4A00  64+10
  unsigned short frameRetain;                       //帧保留 ==0000
  unsigned short GoalId;                            //目的ID ==0B00
  unsigned short SourceID;                          //源ID ==0D00
  unsigned short synchronizing;                     //同步序列 ==FFF5
  unsigned char frameType;                          //帧类型 ==05
  unsigned char frameCnt;                           //帧计数 ==37
  unsigned char TPDPoptioncode;                     //跳频定频操作码 ==C1
  unsigned char TPDPdatecode;                       //跳频定频数据码
  unsigned char ZWFSoptioncode;                     //组网方式操作码 ==C2
  unsigned char ZWFSdatecode;                       //组网方式数据码
  unsigned char QOSoptioncode;                      //QOS等级操作码  ==C3
  unsigned char QOSdatecode;                        //QOS等级数据码
  unsigned char TPMSoptioncode;                     //跳频模式操作码 ==C4
  unsigned char TPMSdatecode;                       //跳频模式数据码
  unsigned char TPSLoptioncode;                     //跳频速率操作码 ==C5
  unsigned char TPSLdatecode;                       //跳频速率数据码 ==00
  unsigned char TPPBoptioncode;                     //跳频频表操作码 ==C6
  unsigned char TPPBdatecode;                       //跳频频表数据码 ==00
  unsigned char TPXLoptoncode;                      //跳频序列操作码 ==C7
  unsigned char TPXLdatecode;                       //跳频序列数据码 ==00
  unsigned char TZDKoptioncode;                     //调制带宽操作码 ==C8
  unsigned char TZDKdatecode;                       //调制带宽数据码 ==00
  unsigned char GLSJoptioncode;                     //功率衰减操作码 ==C9
  unsigned char GLSJdatecode;                       //功率衰减数据码
  unsigned char JSTDZToptioncode;                   //接受通道状态操作码 ==CA
  unsigned char JSTDZTdatecode;                     //接受通道状态数据码
  unsigned char FSTDZToptioncode;                   //发射通道状态操作码 ==CB
  unsigned char FSTDZTdatecode;                     //发射通道状态数据码
  unsigned char TXLXoptioncode;                     //天线类型操作码 ==CC
  unsigned char TXLXdatecode;                       //天线类型数据码
  unsigned char PL1optioncode;                      //频率1000M和100MHz操作码 ==CD
  unsigned char PL1datecode;                        //频率1000M和100MHz数据码
  unsigned char PL2optioncode;                      //频率10M和1MHz操作码 ==CE
  unsigned char PL2datecode;                        //频率10M和1MHz数据码
  unsigned char PL3optioncode;                      //频率100K和10KHz操作码 ==CF
  unsigned char PL3datecode;                        //频率100K和10KHz数据码
  unsigned char PL4optioncode;                      //频率1K和100Hz操作码 ==D0
  unsigned char PL4datecode;                        //频率1K和100Hz数据码
  unsigned char PL5optioncode;                      //频率10和1Hz操作码 ==D1
  unsigned char PL5datecode;                        //频率10和1Hz数据码
  unsigned char IPB1optioncode;                     //IP地址B1操作码 ==D2
  unsigned char IPB1datecode;                       //IP地址B1数据码
  unsigned char IPB2optioncode;                     //IP地址B2操作码 ==D3
  unsigned char IPB2datecode;                       //IP地址B2数据码
  unsigned char IPB3optioncode;                     //IP地址B3操作码 ==D4
  unsigned char IPB3datecode;                       //IP地址B3数据码
  unsigned char IPB4optioncode;                     //IP地址B4操作码 ==D5
  unsigned char IPB4datecode;                       //IP地址B4数据码
  unsigned char IPYMB1optioncode;                   //IP地址掩码B1操作码 ==D6
  unsigned char IPYMB1datecode;                     //IP地址掩码B1数据码
  unsigned char IPYMB2optioncode;                   //IP地址掩码B2操作码 ==D7
  unsigned char IPYMB2datecode;                     //IP地址掩码B2数据码
  unsigned char IPYMB3optioncode;                   //IP地址掩码B3操作码 ==D8
  unsigned char IPYMB3datecode;                     //IP地址掩码B3数据码
  unsigned char IPYMB4optioncode;                   //IP地址掩码B4操作码 ==D9
  unsigned char IPYMB4datecode;                     //IP地址掩码B4数据码
  unsigned char WGDZB1optioncode;                   //网关地址B1操作码 ==DA
  unsigned char WGDZB1datecode;                     //网关地址B1数据码
  unsigned char WGDZB2optioncode;                   //网关地址B2操作码 ==DB
  unsigned char WGDZB2datecode;                     //网关地址B2数据码
  unsigned char WGDZB3optioncode;                   //网关地址B3操作码 ==DC
  unsigned char WGDZB3datecode;                     //网关地址B3数据码
  unsigned char WGDZB4optioncode;                   //网关地址B4操作码 ==DD
  unsigned char WGDZB4datecode;                     //网关地址B4数据码
  unsigned char TZFSoptioncode;                     //调制方式操作码 ==DE
  unsigned char TZFSdatecode;                       //调制方式数据码
  unsigned char XDBMotioncode;                      //信道编码操作码 ==DF
  unsigned char XDBMdatecode;                       //信道编码数据码
}back_jobmode_set;
struct selftestReport           /*定义组网软件返回主控自检查询数据帧格式*/
{
  unsigned short frameHead;                         //帧头 ==0X0F00
  unsigned short frameRetain;                       //帧保留 ==0000
  unsigned short GoalId;                            //目的ID ==0B00
  unsigned short SourceID;                          //源ID ==0D00
  unsigned short synchronizing;                     //同步序列 ==FFF5
  unsigned char frameType;                          //帧类型 ==0E     00001110
  unsigned char frameCnt;                           //帧计数 ==02     00000010
  unsigned char selftestoptioncode;                 //自检操作码 ==e1 11100001
  unsigned char selftestdatecode;                   //自检数据码 ==04
  unsigned char frameEnd;                           //帧尾(即校验和) ==E9
}selftest_report;
struct versionReport           /*定义组网软件返回主控版本查询数据帧格式*/
{
  unsigned short frameHead;                         //帧头 ==0X1300
  unsigned short frameRetain;                       //帧保留 ==0000
  unsigned short GoalId;                            //目的ID ==0B00
  unsigned short SourceID;                          //源ID ==0D00
  unsigned short synchronizing;                     //同步序列 ==FFF5
  unsigned char frameType;                          //帧类型 ==10          00010000
  unsigned char frameCnt;                           //帧计数 ==06          00001111
  unsigned char PLVoptioncode;                      //PL版本操作码 ==E1    11100001
  unsigned char PLVdatecode;                        //PL版本数据码
  unsigned char MACVoptioncode;                     //MAC版本操作码 ==E2   11100010
  unsigned char MACVdatecode;                       //MAC版本数据码
  unsigned char NETVoptioncode;                     //NET版本操作码 ==E3   11100011
  unsigned char NETVdatecode;                       //NET版本数据码
  unsigned char frameEnd;                           //帧尾(即校验和) ==FF  11111111
}version_report;

/*※※※※※※※※※※※※※※※※※※※※※全局变量声明※※※※※※※※※※※※※※※※※※※※※※*/
unsigned int linkCnt = 0;                                   //定义建链标志

/*定义UDP套接字*/
int sockfd;
char BenDiIP[256];
struct sockaddr_in server_addr, client_addr;                //定义服务器、客户端地址结构
struct sockaddr_in dest_addr;
        ///UDP接收线程    UDP发送线程 RAM读取线程 RAM写入线程   网口接收线程  网口发送线程
pthread_t receive_tid , send_tid , read_RAM , write_RAM , receive_wk , send_wk; //定义线程ID

int AllFrameCnt = 0;                                        //定义接收到的数据总数
int ErrorByte = 0;                                          //定义误码数量
double BER = 0;                                             //定义误码率 
int IP1,IP2,IP3,IP4;                                        //定义IP的四个部分
/******************* 函数名称：转换输出二进制****************************
* 功能描述：获取二进制
**********************************************************************/
void printBinary(unsigned int num) {
    char IP[32];
    int i = sizeof(num) * 8 - 1;
    for (i; i >= 0; i--) {
        unsigned int mask = 1 << i;
        IP[31-i] = (num & mask) ? 1 : 0;//将二进制数倒入数组中
        //printf("%d", (num & mask) ? 1 : 0);
    }
    //printf("\n");
    //由于IP转换的规则，后8位二进制数据转换为点分十进制IP的第一位整数，16至23位转换为第二位，8到15转换为第三位，0到7转换为第一位
    IP1 = IP[31] + IP[30]*2 + IP[29]*4 + IP[28]*8 + IP[27]*16 + IP[26]*32 + IP[25]*64 + IP[24]*128;
    IP2 = IP[23] + IP[22]*2 + IP[21]*4 + IP[20]*8 + IP[19]*16 + IP[18]*32 + IP[17]*64 + IP[16]*128;
    IP3 = IP[15] + IP[14]*2 + IP[13]*4 + IP[12]*8 + IP[11]*16 + IP[10]*32 + IP[9]*64 + IP[8]*128;
    IP4 = IP[7] + IP[6]*2 + IP[5]*4 + IP[4]*8 + IP[3]*16 + IP[2]*32 + IP[1]*64 + IP[0]*128;
}
/************************* 函数名称：GetIP******************************
* 功能描述：获取本地IP
**********************************************************************/
void GetIP(void)					
{
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    struct sockaddr_in *sa;
    getifaddrs(&ifaddr);
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
        sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (sa->sin_family == AF_INET) {
            inet_ntop(AF_INET, &sa->sin_addr, BenDiIP, sizeof(BenDiIP));
            // 检查是否是环回接口
            if (!(ifa->ifa_flags & IFF_LOOPBACK)) {  //与上环回接口标志位，如果是环回接口则排除
               in_addr_t ipAddr = inet_addr(BenDiIP);//将点分十进制IP转换成十进制数
               printBinary(ipAddr);                  //将十进制数转换成二进制
                break;
			}
		}
	}
}
/******************* 函数名称：variableInit****************************
* 版本标识：v3.00
* 创建时间：2024年8月22日
* 功能描述：默认初始化变量(初始化返回值防止错误数据)
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：
* ...
* 修改内容n：
* 修改人n：
* 修改时间n：
**********************************************************************/
void variableInit(void)					
{
    memset(&RxNetSpeechData,'\0',sizeof(struct speechdata));//清0缓存
	memset(&jobmode_query,0,sizeof(struct jobmodeQuery));   //清0缓存
	memset(&jobmode_set,0,sizeof(struct jobmodeSet));       //清0缓存
	memset(&TPmode_set,0,sizeof(struct TPmodeSet));         //清0缓存
	memset(&TPmode_set,0,sizeof(struct DPmodeSet));         //清0缓存
	memset(&TPCS_set,0,sizeof(struct TPCSSet));             //清0缓存
	memset(&TZDK_set,0,sizeof(struct TZDKSet));             //清0缓存
	memset(&TZFS_set,0,sizeof(struct TZFSSet));             //清0缓存
	memset(&XDBM_set,0,sizeof(struct XDBMSet));             //清0缓存
	memset(&GLSJ_set,0,sizeof(struct GLSJSet));             //清0缓存
	memset(&WLCS_set,0,sizeof(struct WLCSSet));             //清0缓存
	memset(&TXLX_set,0,sizeof(struct TXLXSet));             //清0缓存
	memset(&selftest_query,0,sizeof(struct selftestQuery)); //清0缓存
	memset(&version_query,0,sizeof(struct versionQuery));   //清0缓存
	memset(&GZPBHF_set,0,sizeof(struct GZPBHFSet));         //清0缓存
	
	/*工作参数回执初始赋值*/
	{
	    back_jobmode_set.frameHead=0x4A00;		            //帧头 ==0X4A00  64+10
	    back_jobmode_set.frameRetain=0x0000;                //帧保留 ==0000
	    back_jobmode_set.GoalId=0x0C00;                     //目的ID ==0C00
	    back_jobmode_set.SourceID=0x0B00;                   //源ID ==0B00
	    back_jobmode_set.synchronizing=0xFFF5;              //同步序列 ==FFF5
	    back_jobmode_set.frameType=0x05;                    //帧类型 ==05
	    back_jobmode_set.frameCnt=0x37;                     //帧计数 ==37
	    back_jobmode_set.TPDPoptioncode=0xC1;               //跳频定频操作码 ==C1
	    back_jobmode_set.TPDPdatecode=0x01;                 //跳频定频数据码
	    back_jobmode_set.ZWFSoptioncode=0xC2;               //组网方式操作码 ==C2
	    back_jobmode_set.ZWFSdatecode=0x02;                 //组网方式数据码
	    back_jobmode_set.QOSoptioncode=0xC3;                //QOS等级操作码  ==C3
	    back_jobmode_set.QOSdatecode=0x02;                  //QOS等级数据码
	    back_jobmode_set.TPMSoptioncode=0xC4;               //跳频模式操作码 ==C4
	    back_jobmode_set.TPMSdatecode=0x01;                 //跳频模式数据码
	    back_jobmode_set.TPSLoptioncode=0xC5;               //跳频速率操作码 ==C5
	    back_jobmode_set.TPSLdatecode=0x05;                 //跳频速率数据码 ==00
	    back_jobmode_set.TPPBoptioncode=0xC6;               //跳频频表操作码 ==C6
	    back_jobmode_set.TPPBdatecode=0x06;                 //跳频频表数据码 ==00
	    back_jobmode_set.TPXLoptoncode=0xC7;                //跳频序列操作码 ==C7
	    back_jobmode_set.TPXLdatecode=0x07;                 //跳频序列数据码 ==00
	    back_jobmode_set.TZDKoptioncode=0xC8;               //调制带宽操作码 ==C8
	    back_jobmode_set.TZDKdatecode=0x08;                 //调制带宽数据码 ==00
	    back_jobmode_set.GLSJoptioncode=0xC9;               //功率衰减操作码 ==C9
	    back_jobmode_set.GLSJdatecode=0x09;                 //功率衰减数据码
	    back_jobmode_set.JSTDZToptioncode=0xCA;             //接受通道状态操作码 ==CA
	    back_jobmode_set.JSTDZTdatecode=0x00;               //接受通道状态数据码
	    back_jobmode_set.FSTDZToptioncode=0xCB;             //发射通道状态操作码 ==CB
	    back_jobmode_set.FSTDZTdatecode=0x00;               //发射通道状态数据码
	    back_jobmode_set.TXLXoptioncode=0xCC;               //天线类型操作码 ==CC
	    back_jobmode_set.TXLXdatecode=0x01;                 //天线类型数据码
	    back_jobmode_set.PL1optioncode=0xCD;                //频率1000M和100MHz操作码 ==CD
	    back_jobmode_set.PL1datecode=0x0D;                  //频率1000M和100MHz数据码
	    back_jobmode_set.PL2optioncode=0xCE;                //频率10M和1MHz操作码 ==CE
	    back_jobmode_set.PL2datecode=0x0E;                  //频率10M和1MHz数据码
	    back_jobmode_set.PL3optioncode=0xCF;                //频率100K和10KHz操作码 ==CF
	    back_jobmode_set.PL3datecode=0x0F;                  //频率100K和10KHz数据码
	    back_jobmode_set.PL4optioncode=0xD0;                //频率1K和100Hz操作码 ==D0
	    back_jobmode_set.PL4datecode=0x00;                  //频率1K和100Hz数据码
	    back_jobmode_set.PL5optioncode=0xD1;                //频率10和1Hz操作码 ==D1
	    back_jobmode_set.PL5datecode=0x01;                  //频率10和1Hz数据码
	    back_jobmode_set.IPB1optioncode=0xD2;               //IP地址B1操作码 ==D2
	    back_jobmode_set.IPB1datecode=0x7F;                 //IP地址B1数据码
	    back_jobmode_set.IPB2optioncode=0xD3;               //IP地址B2操作码 ==D3
	    back_jobmode_set.IPB2datecode=0x00;                 //IP地址B2数据码
	    back_jobmode_set.IPB3optioncode=0xD4;               //IP地址B3操作码 ==D4
	    back_jobmode_set.IPB3datecode=0x00;                 //IP地址B3数据码
	    back_jobmode_set.IPB4optioncode=0xD5;               //IP地址B4操作码 ==D5
	    back_jobmode_set.IPB4datecode=0x01;                 //IP地址B4数据码
	    back_jobmode_set.IPYMB1optioncode=0xD6;             //IP地址掩码B1操作码 ==D6
	    back_jobmode_set.IPYMB1datecode=0xFF;               //IP地址掩码B1数据码
	    back_jobmode_set.IPYMB2optioncode=0xD7;             //IP地址掩码B2操作码 ==D7
	    back_jobmode_set.IPYMB2datecode=0xFF;               //IP地址掩码B2数据码
	    back_jobmode_set.IPYMB3optioncode=0xD8;             //IP地址掩码B3操作码 ==D8
	    back_jobmode_set.IPYMB3datecode=0xFF;               //IP地址掩码B3数据码
	    back_jobmode_set.IPYMB4optioncode=0xD9;             //IP地址掩码B4操作码 ==D9
	    back_jobmode_set.IPYMB4datecode=0x00;               //IP地址掩码B4数据码
	    back_jobmode_set.WGDZB1optioncode=0xDA;             //网关地址B1操作码 ==DA
	    back_jobmode_set.WGDZB1datecode=0x7F;               //网关地址B1数据码
	    back_jobmode_set.WGDZB2optioncode=0xDB;             //网关地址B2操作码 ==DB
	    back_jobmode_set.WGDZB2datecode=0x00;               //网关地址B2数据码
	    back_jobmode_set.WGDZB3optioncode=0xDC;             //网关地址B3操作码 ==DC
	    back_jobmode_set.WGDZB3datecode=0x00;               //网关地址B3数据码
	    back_jobmode_set.WGDZB4optioncode=0xDD;             //网关地址B4操作码 ==DD
	    back_jobmode_set.WGDZB4datecode=0x0A;               //网关地址B4数据码
	    back_jobmode_set.TZFSoptioncode=0xDE;               //调制方式操作码 ==DE
	    back_jobmode_set.TZFSdatecode=0x0E;                 //调制方式数据码
	    back_jobmode_set.XDBMotioncode=0xDF;                //信道编码操作码 ==DF
	    back_jobmode_set.XDBMdatecode=0x0F;                 //信道编码数据码
	}                                                      
    
	/*自检信息初始赋值*/      
	{	
	    selftest_report.frameHead = 0x0F00;                 //帧头 ==0X0F00      
	    selftest_report.frameRetain = 0x0000;               //帧保留 ==0000      
	    selftest_report.GoalId = 0x0C00;                    //目的ID ==0C00      
	    selftest_report.SourceID = 0x0B00;                  //源ID ==0B00        
        selftest_report.synchronizing = 0xFFF5;             //同步序列 ==FFF5    
        selftest_report.frameType = 0x0E;                   //帧类型 ==0E
        selftest_report.frameCnt = 0x02;                    //帧计数 ==02
        selftest_report.selftestoptioncode = 0xE1;          //自检操作码 ==e1 
        selftest_report.selftestdatecode = 0x04;            //自检数据码 ==04
	}
	
	/*版本信息初始赋值*/ 
	{
		version_report.frameHead=0x1300;		            //帧头 ==0X1300 
		version_report.frameRetain=0x0000;                  //帧保留 ==0000 
		version_report.GoalId=0x0C00;                       //目的ID ==0C00 
		version_report.SourceID=0x0B00;                     //源ID ==0B00   
		version_report.synchronizing=0xFFF5;                //同步序列 ==FFF5
		version_report.frameType=0x10;                      //帧类型 ==0x10
		version_report.frameCnt=0x0F;                       //帧计数 ==0x0F
		version_report.PLVoptioncode=0xE1;                  //PL版本操作码 ==E1
		version_report.PLVdatecode=0x01;                    //PL版本数据码 ==01
		version_report.MACVoptioncode=0xE2;                 //MAC版本操作码 ==e2
		version_report.MACVdatecode=0x01;                   //MAC版本数据码 ==01
		version_report.NETVoptioncode=0xE3;                 //NET版本操作码 ==e3
		version_report.NETVdatecode=0x01;                   //NET版本数据码 ==01
	}
}
/******************** 函数名称：makeSendData*****************************
* 版本标识：v3.00
* 创建时间：2024年8月22日
* 功能描述：完成对网络发送数据的帧构建
* 函数输入：makeclasses（构建数据类别，1建链回执帧，2肯定应答回执，3否定应答回执，4工作参数查询回执，
*                        5自检查询回执，6版本查询回执）
* 修改内容1：
* 修改人1：
* 修改时间1：
* ...
* 修改内容n：
* 修改人n：
* 修改时间n：
**********************************************************************/
void makeSendData(unsigned char makeclasses)
{
	switch(makeclasses)
	{
	case 1:                                                 //构建建链回执帧
	{
		linkCnt++;                                          //建链计数
		break;                                              //跳出循环
	}
	case 2:                                                 //构建肯定应答回执帧
	{
		positive_ack.frameHead=0x0D00;	                    //帧头
		positive_ack.frameRetain=0x0000;                    //帧保留
		positive_ack.GoalId=0x0B00;                         //目的ID
		positive_ack.SourceID=0x0A00;                       //源ID
		positive_ack.synchronizing=0xFFF5;                  //同步序列
		positive_ack.frameType=0x03;                        //帧类型
		positive_ack.frameCnt=0x00;		  	                //帧计数
		positive_ack.frameEnd=0x03;                         //帧尾（校验和）
		break;
	}
	case 3:                                                 //构建否定应答回执帧
	{
		negative_ack.frameHead=0x0D00;
		negative_ack.frameRetain=0x0000;
		negative_ack.GoalId=0x0B00;
		negative_ack.SourceID=0x0A00;
		negative_ack.synchronizing=0xFFF5;
		negative_ack.frameType=0x04;
		negative_ack.frameCnt=0x00;					
		negative_ack.frameEnd=0x04;
		break;
	}
	case 4:                                                 //构建工作参数查询回执帧
	{
		back_jobmode_set.TPDPdatecode=jobmode_set.TPDPdatecode;    //跳频定频数据码统一
		back_jobmode_set.ZWFSdatecode=jobmode_set.ZWFSdatecode;    //组网方式数据码统一
		back_jobmode_set.QOSdatecode=jobmode_set.QOSdatecode;      //QOS等级 数据码统一
		back_jobmode_set.TPMSdatecode=TPmode_set.TPmodedatecode;   //跳频数据码修正
		back_jobmode_set.PL1datecode=DPmode_set.DPPL1datecode;     //定频频率修正
		back_jobmode_set.PL2datecode=DPmode_set.DPPL2datecode;
		back_jobmode_set.PL3datecode=DPmode_set.DPPL3datecode;
		back_jobmode_set.PL4datecode=DPmode_set.DPPL4datecode;
		back_jobmode_set.PL5datecode=DPmode_set.DPPL5datecode;
		back_jobmode_set.TPSLdatecode=TPCS_set.TPSLdatecode;       //跳频速率数据码统一
		back_jobmode_set.TPPBdatecode=TPCS_set.TPPBdatecode;       //跳频频表数据码统一
		back_jobmode_set.TPXLdatecode=TPCS_set.TPXLdatecode;       //跳频序列数据码统一
		back_jobmode_set.TZDKdatecode=TZDK_set.TZKDdatecode;       //调制带宽数据码修正
		back_jobmode_set.TZFSdatecode=TZFS_set.TZFSdatecode;       //调制方式数据码修正
		back_jobmode_set.XDBMdatecode=XDBM_set.XDBMdatecode;       //信道编码数据码修正
		back_jobmode_set.GLSJdatecode=GLSJ_set.GLSJdatecode;       //功率衰减数据码修正
		back_jobmode_set.IPB1datecode=WLCS_set.IPB1datecode;       //网络IP数据码修正
		back_jobmode_set.IPB2datecode=WLCS_set.IPB2datecode;
		back_jobmode_set.IPB3datecode=WLCS_set.IPB3datecode;
		back_jobmode_set.IPB4datecode=WLCS_set.IPB4datecode;
		back_jobmode_set.IPYMB1datecode=WLCS_set.IPYMB1datecode;   //地址掩码数据码修正
		back_jobmode_set.IPYMB2datecode=WLCS_set.IPYMB2datecode;
		back_jobmode_set.IPYMB3datecode=WLCS_set.IPYMB3datecode;
		back_jobmode_set.IPYMB4datecode=WLCS_set.IPYMB4datecode;
		back_jobmode_set.WGDZB1datecode=WLCS_set.WGDZB1datecode;   //网关地址数据码修正
		back_jobmode_set.WGDZB2datecode=WLCS_set.WGDZB2datecode;
		back_jobmode_set.WGDZB3datecode=WLCS_set.WGDZB3datecode;
		back_jobmode_set.WGDZB4datecode=WLCS_set.WGDZB4datecode;
		back_jobmode_set.TXLXdatecode=TXLX_set.TXLXdatecode;       //天线类型数据码修正
		break;
	}
	case 5:                                                 //构建自检查询回执帧
	{
        selftest_report.selftestdatecode = 0x04;            //自检数据码 ==04    
        selftest_report.frameEnd = (selftest_report.frameType ^ selftest_report.frameCnt ^ selftest_report.selftestoptioncode ^ selftest_report.selftestdatecode);            //帧尾(即校验和)
		break;
	}
	case 6:                                                 //构建版本查询回执帧
	{
		version_report.PLVdatecode=0x01;                    //PL版本数据码 ==01
		version_report.MACVdatecode=0x01;                   //MAC版本数据码 ==01
		version_report.NETVdatecode=0x01;                   //NET版本数据码 ==01
		version_report.frameEnd = (version_report.frameType ^ version_report.frameCnt ^ version_report.PLVoptioncode ^ version_report.PLVdatecode ^
		                           version_report.MACVoptioncode ^ version_report.MACVdatecode ^ version_report.NETVoptioncode ^ version_report.NETVdatecode);
		break;
	}
	default : makeclasses = 0;                              //结束一轮while循环
	}
}
/******** 函数名称：recJobmodeQuery（存在问题：工作模式指令尚未定义）********
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web工作参数查询udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recJobmodeQuery(void)
{                                                                  //判定是否为工作模式查询数据
    if(jobmode_query.frameHead == 0x0E && jobmode_query.frameType == 0X01 && jobmode_query.queryOptioncode == 0xD0 )
	{
		sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
				
	}
}
/**********************函数名称：recJobmodeSet**************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web工作模式配置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recJobmodeSet(void)
{
	if(jobmode_set.frameHead == 0x13 && jobmode_set.frameType == 0X01 && jobmode_set.frameCnt == 0x08)
	{                                                            
		makeSendData(4);                                           //构建工作参数查询回执帧
		sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		makeSendData(2);                                           //构建应答数据
		sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		
	}
}
/********************* 函数名称：recTPmodeSet**************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web跳频模式配置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recTPmodeSet(void)
{
    if(TPmode_set.frameHead == 0x0F && TPmode_set.frameType == 0X05 
	&& TPmode_set.frameCnt == 0x02 && TPmode_set.TPmodeoptioncode == 0xC4 )
	{
		makeSendData(4);
		sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		makeSendData(2);   
		sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
				
	}
}
/**********************函数名称：recDPmodeSet***************************
***************** 功能描述：接收web定频配置udp指令处理*********************
**********************************************************************/
void recDPmodeSet(void)
{
    if(DPmode_set.frameHead == 0x17 && DPmode_set.frameType == 0X06 && DPmode_set.frameCnt == 0x0A)
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/************************函数名称：recTPCSSet***************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web跳频参数预置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recTPCSSet(void)
{
    if(TPCS_set.frameHead == 0x13 && TPCS_set.frameType == 0X07 && TPCS_set.frameCnt == 0x06)
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/***********************函数名称：recTZDKSet***************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web调制带宽配置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recTZDKSet(void)
{
    if(TZDK_set.frameHead == 0x0F && TZDK_set.frameType == 0X08 
	&& TZDK_set.frameCnt == 0x02 && TZDK_set.TZKDoptioncode == 0xC8 )
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/**********************函数名称：recTZFSSet****************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web调制方式配置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recTZFSSet(void)
{
    if(TZFS_set.frameHead == 0x0F && TZFS_set.frameType == 0X09 
	&& TZFS_set.frameCnt == 0x02 && TZFS_set.TZFSoptioncode == 0xDE )
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/************************函数名称：recXDBMSet***************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web信道编码配置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recXDBMSet(void)
{
    if(XDBM_set.frameHead == 0x0F && XDBM_set.frameType == 0X0A 
	&& XDBM_set.frameCnt == 0x02 && XDBM_set.XDBMoptioncode == 0xDF )
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/*************************函数名称：recGLSJSet**************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web信道功率衰减配置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recGLSJSet(void)
{
    if(GLSJ_set.frameHead == 0x0F && GLSJ_set.frameType == 0X0B 
	&& GLSJ_set.frameCnt == 0x02 && GLSJ_set.GLSJoptioncode == 0xC9 )
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/*************************函数名称：recWLCSSet*************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web网络参数配置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recWLCSSet(void)
{
    if(WLCS_set.frameHead == 0x25 && WLCS_set.frameType == 0X0C && WLCS_set.frameCnt == 0x18)
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/*************************函数名称：recTXLXSet*************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web天线类型设置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recTXLXSet(void)
{
    if(TXLX_set.frameHead == 0x0F && TXLX_set.frameType == 0X11 
	&& TXLX_set.frameCnt == 0x02)
	{
		    makeSendData(4);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(2);   
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}
/******函数名称：recSelftestQuery（存在问题:返回web的自检数据码尚未定义）*****
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web自检查询udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recSelftestQuery(void)
{
    if(selftest_query.frameHead == 0x0E && selftest_query.frameType == 0x0D  && selftest_query.frameCnt == 0x01)
	{
        
		    makeSendData(5);
        printf("shu_ju_yi_gou_jian\n");
			sendto(sockfd,&selftest_report,sizeof(struct selftestReport),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		
	}
}
/****函数名称：recVersionQuery（存在问题:返回web的版本查询数据码尚未定义）****
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web版本查询udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recVersionQuery(void)
{
    if(version_query.frameHead == 0x0E && version_query.frameType == 0X0F && version_query.frameCnt == 0x01)
	{
		    makeSendData(6);
        printf("shu_ju_yi_gou_jian\n");
			sendto(sockfd,&version_report,sizeof(struct versionReport),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		
	}
}
/*****函数名称：recGZPBHFSet（存在问题:接收web故障屏蔽与恢复设置后无返回数据）***
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web故障屏蔽与恢复设置udp指令处理
* 函数输入：无
* 函数输出：无
* 修改内容1：
* 修改人1：
* 修改时间1：			
**********************************************************************/
void recGZPBHFSet(void)
{
    if(GZPBHF_set.frameHead == 0x0F && GZPBHF_set.frameType == 0X12 && GZPBHF_set.frameCnt == 0x02 && GZPBHF_set.GZoptioncode == 0xE4	)
	{
		    makeSendData(2);
        printf("shu_ju_yi_gou_jian\n");
			sendto(sockfd,&positive_ack,sizeof(struct positiveAck),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
			
	}
}


/*********************************************************************
* 函数名称：udp_receive_thread
* 创建时间：2026年4月10日
* 功能描述：实现UDP数据的接收处理
**********************************************************************/
static void *udp_receive_thread(void *arg) {   
    unsigned char buffer[BUF_SIZE];
    unsigned char rx_udp_type;
    unsigned int rx_udp_cnt = 0;
    unsigned char SourceID;
    unsigned char GoalId;
    unsigned char FrameEnd;
    unsigned char jiaoyan;
    socklen_t addr_len;
    
	//创建udp_receive_thread接收线程UDP套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);//SOCK_DGRAM表示UDP传输
    //设置服务器地址结构
    server_addr.sin_family = AF_INET;               //AF_INET表示IPv4
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);//htonl将一个 32 位的无符号整数从主机字节序转换为网络字节序
	//INADDR_ANY表示服务器可以接收来自任何网络接口的连接请求也就是说，服务器不绑定到特定的 IP 地址，而是监听所有可用的本地 IP 地址。
    server_addr.sin_port = htons(SELF_PORT);        //表示协定接收端口
    bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));// 绑定套接字
	addr_len = sizeof(client_addr);                 //将客户端地址结构长度赋予addr_len
    while(1) {
	    if((rx_udp_cnt = recvfrom(sockfd, buffer, BUF_SIZE, 0, (struct sockaddr *)&client_addr, &addr_len)) < 0){
			perror("recvfrom");
		} else{
			AllFrameCnt++;
			}//记录接受到的数据帧数量
	    int x = 10;
	    jiaoyan = buffer[x];		
	    for (x = 11;x < rx_udp_cnt - 1;x++){  //计算接收数据的校验和
		    jiaoyan = buffer[x] ^ jiaoyan;    
		    FrameEnd = buffer[x+1];           //取接收到数据帧帧尾的校验和
		}
		if(jiaoyan != FrameEnd){
           ErrorByte++;		   //记录收到的错误帧数量
		   continue;
        }
	    //计算误码率
	    BER = ErrorByte/AllFrameCnt;
		
	    //打印出收到的数据
	    printf("Received data: ");
        int i;
        for (i = 0; i < rx_udp_cnt; i++) {
             printf("%02x ", buffer[i]);
	    	}printf("\n");
			
	    //printf("Received len: rx_udp_cnt = %d\n", rx_udp_cnt);      //打印看接收到的长度是多少
        //printf("this_type_len:%ld\n", sizeof(struct selftestQuery));//打印看自检查询结构体的长度是多少
			
        //判断数据帧类型
        int j = 10;
        rx_udp_type = buffer[j];
	    //printf("rec type: 0x%02x\n", rx_udp_type);                  //打印看返回数据type的值
		
	    //判断接受的数据是否是来自网管且发往主控
	    int m = 4;
        GoalId = buffer[m];  //buffer[4]表示数据目的地址号，主控为b
	    int s = 6;
        SourceID = buffer[s];//buffer[6]表示数据来源地址号，web网管为c
	    if(GoalId == 0x0b && SourceID == 0x0c )
        {       
            if(rx_udp_cnt == 14 &&  rx_udp_type == 0x01)
	    	{
                printf("recive jobmodeQuery \n");
	    		memcpy(&jobmode_query,buffer,rx_udp_cnt);//缓存web工作参数查询数据
	    		recJobmodeQuery();                       //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 19  &&  rx_udp_type == 0x01)
	    	{
                printf("recive jobmodeSet \n");
	    		memcpy(&jobmode_set,buffer,rx_udp_cnt);  //缓存web工作模式配置指令数据
	    		recJobmodeSet();                         //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 15  &&  rx_udp_type == 0x05)
	    	{
                printf("recive TPmodeSet \n");
	    		memcpy(&TPmode_set,buffer,rx_udp_cnt);   //缓存web跳频模式配置指令数据
	    		recTPmodeSet();                          //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 23  &&  rx_udp_type == 0x06)
	    	{
                printf("recive DPmodeSet \n");
	    		memcpy(&DPmode_set,buffer,rx_udp_cnt);   //缓存web定频配置指令数据
	    		recDPmodeSet();                          //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 19  &&  rx_udp_type == 0x07)
	    	{
                printf("recive TPCSSet \n");
	    		memcpy(&TPCS_set,buffer,rx_udp_cnt);     //缓存web跳频参数预设指令数据
	    		recTPCSSet();                            //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 15  &&  rx_udp_type == 0x08)
	    	{
                printf("recive TZDKSet \n");
	    		memcpy(&TZDK_set,buffer,rx_udp_cnt);     //缓存web调制带宽配置指令数据
	    		recTZDKSet();                            //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 15  &&  rx_udp_type == 0x09)
	    	{
                printf("recive TZFSSet \n");
	    		memcpy(&TZFS_set,buffer,rx_udp_cnt);     //缓存web调制方式配置指令数据
	    		recTZFSSet();                            //处理指令
                rx_udp_type = 0;                         //重置rx_udp_type
	    	}else if(rx_udp_cnt == 15  &&  rx_udp_type == 0x0A)
	    	{
                printf("recive XDBMSet \n");
	    		memcpy(&XDBM_set,buffer,rx_udp_cnt);     //缓存web信道编码配置指令数据
	    		recXDBMSet();                            //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 15  &&  rx_udp_type == 0x0B)
	    	{
                printf("recive GLSJSet \n");
	    		memcpy(&GLSJ_set,buffer,rx_udp_cnt);     //缓存web信道功率衰减配置指令数据
	    		recGLSJSet();                            //处理指令
                rx_udp_type = 0;                         //重置rx_udp_type
	    	}else if(rx_udp_cnt == 37  &&  rx_udp_type == 0x0C)
	    	{
                printf("recive WLCSSet \n");
	    		memcpy(&WLCS_set,buffer,rx_udp_cnt);     //缓存web网络参数配置指令数据
	    		recWLCSSet();                            //处理指令
                rx_udp_type = 0;                         //重置rx_udp_type
	    	}else if(rx_udp_cnt == 15  &&  rx_udp_type == 0x11)
	    	{
                printf("recive TXLXSet \n");
	    		memcpy(&TXLX_set,buffer,rx_udp_cnt);     //缓存web天线类型设置指令数据
	    		recTXLXSet();                            //处理指令
                rx_udp_type = 0;                         //重置rx_udp_type
	    	}else if(rx_udp_cnt == 14  &&  rx_udp_type == 0x0D)
	    	{
                printf("recive selftestQuery \n");
	    		memcpy(&selftest_query,buffer,rx_udp_cnt);//缓存web自检查询指令数据
	    		recSelftestQuery();                       //处理指令
                rx_udp_type = 0;                          // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 14  &&  rx_udp_type == 0x0F)
	    	{
                printf("recive versionQuery \n");
	    		memcpy(&version_query,buffer,rx_udp_cnt);//缓存web版本查询指令数据
	    		recVersionQuery();                       //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}else if(rx_udp_cnt == 15  &&  rx_udp_type == 0x12)
	    	{
                printf("recive GZPBHFSet \n");           
	    		memcpy(&GZPBHF_set,buffer,rx_udp_cnt);   //缓存web故障屏蔽与恢复设置指令数据
	    		recGZPBHFSet();                          //处理指令
                rx_udp_type = 0;                         // 重置rx_udp_type
	    	}
	    	else
	    	{
                printf("????date???? \n");
	    		rx_udp_cnt = 0;
	    		memset(&buffer,0, sizeof(buffer));
	    	}
        } else{
            printf("Data not belong zhukong\n");
            }
		usleep(10*1000);
    }
    rx_udp_cnt = 0;
	memset(&buffer,0, sizeof(buffer));
	
    close(sockfd);
    return NULL;
}

/*********************************************************************
* 函数名称：udp_send_thread
* 创建时间：2024年9月4日
* 功能描述：实现UDP发送线程UDP套接字
**********************************************************************/
static void *udp_send_thread(void *arg) {  
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);//创建udp_send_thread发送线程UDP套接字
    // 设置目标地址结构
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(WEB_ADDR); // 发送到地址
    dest_addr.sin_port = htons(WEB_PORT);
    while(1) {
        //sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));// 发送数据
        //sleep(2); // 每隔2秒发送一次数据
		usleep(100*1000);
    }
    close(sockfd);
    return NULL;
}

/*********************************************************************
* 函数名称：main
* 创建时间：2024年4月10日
* 功能描述：主函数，程序入口，完成初始化、装配状态加载和线程创建
**********************************************************************/
int main(int argc, char *argv[]){
	variableInit();                                                            //默认初始化变量

    GetIP();	                                                            
    printf("%d.%d.%d.%d",IP1,IP2,IP3,IP4);                                  
    printf("\n");                                                           
      
   
    if(pthread_create(&send_tid, NULL, udp_send_thread, NULL) == -1)           //创建UDP发送线程
	{                                                                       
        printf("create ZhuKong udp_send_thread pthread failed! \n");        
        return 0;                                                           
    }else printf("create ZhuKong udp_send_thread pthread succeed! \n");     
	
	if(pthread_create(&receive_tid, NULL, udp_receive_thread, NULL) == -1)    //创建UDP接收线程
	{
        printf("create ZhuKong udp_receive_thread pthread failed! \n");
        return 0;
    }else printf("create ZhuKong udp_receive_thread pthread succeed! \n");	
	

    pthread_join(receive_tid, NULL);//等待接收线程结束
    pthread_join(send_tid, NULL); //等待发送进程结束 
    return 0;
}