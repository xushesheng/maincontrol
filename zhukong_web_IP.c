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
struct positiveAck              /*定义主控返回web网管肯定应答数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0D00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==30
  unsigned char frameCnt;                                   //帧计数 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和) ==30
}positive_ack;
struct negativeAck              /*定义主控返回web网管否定应答数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0D00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==40
  unsigned char frameCnt;                                   //帧计数 ==00
  unsigned char frameEnd;                                   //帧尾(即校验和) ==40
}negative_ack;
struct backjobmodeQuery         /*定义主控返回web网管工作参数查询结果数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X4A00  64+10
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char  frameType;                                 //帧类型 ==05
  unsigned char  frameCnt;                                  //帧计数 ==37
  unsigned char  SiteAttribute; 							//分为主从
  unsigned char  NodeName;      							//节点名
  unsigned char  NodeID;        							//节点ID
  unsigned char  IPB1datecode;                              //IP地址B1数据码
  unsigned char  IPB2datecode;                              //IP地址B2数据码
  unsigned char  IPB3datecode;                              //IP地址B3数据码
  unsigned char  IPB4datecode;                              //IP地址B4数据码
  unsigned char  IPYMB1datecode;                            //IP地址掩码B1数据码
  unsigned char  IPYMB2datecode;                            //IP地址掩码B2数据码
  unsigned char  IPYMB3datecode;                            //IP地址掩码B3数据码
  unsigned char  IPYMB4datecode;                            //IP地址掩码B4数据码
  unsigned char  WGDZB1datecode;                            //网关地址B1数据码
  unsigned char  WGDZB2datecode;                            //网关地址B2数据码
  unsigned char  WGDZB3datecode;                            //网关地址B3数据码
  unsigned char  WGDZB4datecode;                            //网关地址B4数据码
  unsigned short HopRate;      								//跳频速率0-2000
  unsigned char  SynSignal;     							//同步信号灯
  unsigned char  LinkQuality;								//链路质量信号
  unsigned char  FaultSignal;								//故障信号灯
  unsigned char  Silent;									//当前设备静默辐射信号
  unsigned char  AllSlient;									//全局静默辐射
  unsigned char  ChannelInfo;   							//信道运行状态
  unsigned short ChannelTemp;  								//16进制+BCD码（第一节子高位16进制表示正负，其余BCD码表示）
  unsigned char  ChannelVolt;								//电压
  unsigned char  ChannelElect;								//电流
  unsigned char  RFInfo;        							//射频运行状态
  unsigned short RFTemp;       								//16进制+BCD码（第一节子高位16进制表示正负，其余BCD码表示）
  unsigned char  RFVolt;									//电压
  unsigned char  RFElect;									//电流
  unsigned char  BasedInfo;     							//基带运行状态
  unsigned short BasedTemp;    								//16进制+BCD码（第一节子高位16进制表示正负，其余BCD码表示）
  unsigned char  BasedVolt;									//
  unsigned char  BasedElect;								//
  unsigned char  Power1Info;    							//功放1运行状态
  unsigned short Power1Temp;   								//16进制+BCD码（第一节子高位16进制表示正负，其余BCD码表示）
  unsigned char  Power1Volt;								//
  unsigned char  Power1Elect;								//
  unsigned char  Power2Info;    							//功放2运行状态
  unsigned short Power2Temp;   								//16进制+BCD码（第一节子高位16进制表示正负，其余BCD码表示）
  unsigned char  Power2Volt;								//
  unsigned char  Power2Elect;								//
  unsigned char  BandwidthSet;								//带宽档位设置
  unsigned char  PowerSet;									//功放档位设置
  unsigned char  Encryption;								//加密方式
  unsigned char  WorkMode;									//工作模式
  unsigned int   FixedFrequency; 							//定频频率
  unsigned int   AdaHopMinFre;   							//自适应跳频起始频率
  unsigned int   AdaHopMaxFre;   							//自适应跳频结束频率
  unsigned char  NotAdaHopFre;  							//非自适应跳频频表
  unsigned char  ComNetName;    							//通信网络名称
  unsigned int   MinFreThreshold;							//最小定频或自适应选频频率阈值
  unsigned int   MaxFreThreshold;							//最大
  unsigned char  Modulation;    							//调制方式
  unsigned char  OnlineNodeSum; 							//在线节点数
  unsigned char  ComDataSum;    							//通信数据总数
  unsigned char  ComDataBER;    							//通信误码率
  unsigned char  ComDataPLP;    							//通信丢包率
  unsigned char  NodeConnect1;  							//节点连接情况(16进制)
  unsigned char  NodeConnect2;
  unsigned char  NodeConnect3;
  unsigned char  NodeConnect4;
  unsigned char  NodeConnect5;
  unsigned char  NodeConnect6;
  unsigned char  NodeConnect7;
  unsigned char  NodeConnect8;
  unsigned char  NodeConnect9;
  unsigned char  NodeConnect10;
  unsigned char  NodeConnect11;
  unsigned char  NodeConnect12;
  unsigned char  NodeConnect13;
  unsigned char  NodeConnect14;
  unsigned char  NodeConnect15;
  unsigned char  NodeConnect16;
  unsigned char  NodeConnect17;
  unsigned char  NodeConnect18;
  unsigned char  NodeConnect19;
  unsigned char  NodeConnect20;
  unsigned char  NodeConnect21;
  unsigned char  NodeConnect22;
  unsigned char  NodeConnect23;
  unsigned char  NodeConnect24;
  unsigned char  NodeConnect25;
  unsigned char  NodeConnect26;
  unsigned char  NodeConnect27;
  unsigned char  NodeConnect28;
  unsigned char  NodeConnect29;
  unsigned char  NodeConnect30;
  unsigned char  NodeConnect31;
  unsigned char  NodeConnect32;
  unsigned char  NodeConnect33;
  unsigned char  NodeConnect34;
  unsigned char  NodeConnect35;
  unsigned char  NodeConnect36;
  unsigned char  NodeConnect37;
  unsigned char  NodeConnect38;
  unsigned char  NodeConnect39;
  unsigned char  NodeConnect40;
  unsigned char  NodeConnect41;
  unsigned char  NodeConnect42;
  unsigned char  NodeConnect43;
  unsigned char  NodeConnect44;
  unsigned char  NodeConnect45;
  unsigned char  NodeConnect46;
  unsigned char  NodeConnect47;
  unsigned char  NodeConnect48;
  unsigned char  NodeConnect49;
  unsigned char  NodeConnect50;
  unsigned char  NodeConnect51;
  unsigned char  NodeConnect52;
  unsigned char  NodeConnect53;
  unsigned char  NodeConnect54;
  unsigned char  NodeConnect55;
  unsigned char  NodeConnect56;
  unsigned char  NodeConnect57;
  unsigned char  NodeConnect58;
  unsigned char  NodeConnect59;
  unsigned char  NodeConnect60;
  unsigned char  NodeConnect61;
  unsigned char  NodeConnect62;
  unsigned char  frameEnd;
}back_jobmode_set;
struct versionReport            /*定义主控返回web网管版本查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1300
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0C00
  unsigned short SourceID;                                  //源ID ==0B00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==10          00001010
  unsigned char frameCnt;                                   //帧计数 ==05          00000101
  unsigned char WEBVersion;                               //PL版本数据码
  unsigned char MCVersion;                                //MAC版本数据码
  unsigned char NETVersion;                               //NET版本数据码
  unsigned char SPCLVersion;
  unsigned char JDCLVersion;
  unsigned char frameEnd;                                   //帧尾(即校验和) ==FF  11111111
}version_report;


/*※※※※※※※※※※※※※※※※※※※web-->主控信息※※※※※※※※※※※※※※※※※※※※*/
struct jobmodeQuery             /*定义接收web网管工作参数查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0D00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==01
  unsigned char frameCnt;                                   //帧计数 ==02
  unsigned char querycode;                              	//查询请求数据码02
  unsigned char frameEnd;                                   //帧尾(即校验和)
}jobmode_query;
struct versionQuery            /*定义接收web网管版本查询数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X0E00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0F
  unsigned char frameCnt;                                   //帧计数 ==01
  unsigned char querycode;                     				//版本查询数据码 ==01
  unsigned char frameEnd;                                   //帧尾(即校验和)
}version_query;
struct WLCSSet                 /*定义接收web网管网络参数配置指令数据帧格式*/
{
  unsigned short frameHead;                                 //帧头 ==0X1B00
  unsigned short frameRetain;                               //帧保留 ==0000
  unsigned short GoalId;                                    //目的ID ==0B00
  unsigned short SourceID;                                  //源ID ==0C00
  unsigned short synchronizing;                             //同步序列 ==FFF5
  unsigned char frameType;                                  //帧类型 ==0C
  unsigned char frameCnt;                                   //帧计数 ==0F
  unsigned short NodeName;
  unsigned char NodeID;
  unsigned char IPB1datecode;                               //IP地址B1数据码
  unsigned char IPB2datecode;                               //IP地址B2数据码
  unsigned char IPB3datecode;                               //IP地址B3数据码
  unsigned char IPB4datecode;                               //IP地址B4数据码
  unsigned char IPYMB1datecode;                             //IP地址掩码B1数据码
  unsigned char IPYMB2datecode;                             //IP地址掩码B2数据码
  unsigned char IPYMB3datecode;                             //IP地址掩码B3数据码
  unsigned char IPYMB4datecode;                             //IP地址掩码B4数据码
  unsigned char WGDZB1datecode;                             //网关地址B1数据码
  unsigned char WGDZB2datecode;                             //网关地址B2数据码
  unsigned char WGDZB3datecode;                             //网关地址B3数据码
  unsigned char WGDZB4datecode;                             //网关地址B4数据码
  unsigned char frameEnd;                                   //帧尾(即校验和)
}WLCS_set;

/*※※※※※※※※※※※※※※※※※※※※※全局变量声明※※※※※※※※※※※※※※※※※※※※※※*/
unsigned int linkCnt = 0;                                   //定义建链标志

/*定义UDP套接字*/
int sockfd;
char BenDiIP[256];
struct sockaddr_in server_addr, client_addr;                //定义服务器、客户端地址结构
struct sockaddr_in dest_addr;
        ///UDP接收线程    UDP发送线程 RAM读取线程 RAM写入线程   网口接收线程  网口发送线程
pthread_t receive_tid , send_tid ; //定义线程ID

int AllFrameCnt = 0;                                        //定义接收到的数据总数
int ErrorByte = 0;                                          //定义误码数量
double BER = 0;                                             //定义误码率 
int IP1,IP2,IP3,IP4;                                        //定义IP的四个部分

/*********************************************************************
* 函数名称：异或校验验证函数
* 功能描述：计算接收数据的头区域数据域异或结果
**********************************************************************/
unsigned char xorChecksum(const void *pkt, size_t size) {
    unsigned char checksum = 0;
    const unsigned char *bytePtr = (const unsigned char*)pkt;
    
    // 注意：排除最后一个字节(frameEnd)不参与计算
    // 因为frameEnd本身就是用来存放校验值的
    for(size_t i = 10; i < size - 1; i++) {
        checksum ^= bytePtr[i];
    }
    
    return checksum;
}
/*********************************************************************
* 函数名称：转换输出二进制
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
/*********************************************************************
* 函数名称：GetIP
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
/*********************************************************************
* 函数名称：variableInit
* 创建时间：2024年8月22日
* 功能描述：默认初始化变量(初始化返回值防止错误数据)
**********************************************************************/
void variableInit(void)					
{
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
	memset(&back_jobmode_set,0,sizeof(struct backjobmodeQuery));
	memste(&version_report,0,sizeof(struct versionReport));
	
	/*工作参数回执初始赋值*/
	{
	    back_jobmode_set.frameHead=0x9800;		            //帧头 ==0X49800  8C+12
	    back_jobmode_set.frameRetain=0x0000;                //帧保留 ==0000
	    back_jobmode_set.GoalId=0x0C00;                     //目的ID ==0C00
	    back_jobmode_set.SourceID=0x0B00;                   //源ID ==0B00
	    back_jobmode_set.synchronizing=0xFFF5;              //同步序列 ==FFF5
	    back_jobmode_set.frameType=0x20;                    //帧类型 ==20
	    back_jobmode_set.frameCnt=0x8C;                     //帧计数 ==8C
	}                                                      
    
	
	/*版本信息回执初始赋值*/ 
	{
		version_report.frameHead=0x1100;		            //帧头 ==0X1100 
		version_report.frameRetain=0x0000;                  //帧保留 ==0000 
		version_report.GoalId=0x0C00;                       //目的ID ==0C00 
		version_report.SourceID=0x0B00;                     //源ID ==0B00   
		version_report.synchronizing=0xFFF5;                //同步序列 ==FFF5
		version_report.frameType=0x10;                      //帧类型 ==0x10
		version_report.frameCnt=0x05;                       //帧计数 ==0x05
	}
}
/******************** 函数名称：makeSendData*****************************
* 版本标识：v3.00
* 创建时间：2024年8月22日
* 功能描述：完成对网络发送数据的帧构建(用从组网接收到的数据构建）
* 函数输入：makeclasses（构建数据类别:1肯定应答回执，2否定应答回执，3工作参数查询回执，4版本查询回执）
**********************************************************************/
void makeSendData(unsigned char makeclasses)
{
	switch(makeclasses)
	{
		case 1:                                                 //构建肯定应答回执帧
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
		case 2:                                                 //构建否定应答回执帧
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
		case 3:                                                 //构建工作参数查询回执帧
		{
			back_jobmode_set.SiteAttribute=0;
			back_jobmode_set.NodeName=0;
			back_jobmode_set.NodeID;
			back_jobmode_set.IPB1datecode=0x00;//实际上要用从共享内存读到的数据构建    //网络IP数据码修正
			back_jobmode_set.IPB2datecode=0x00;
			back_jobmode_set.IPB3datecode=0x00;
			back_jobmode_set.IPB4datecode=0x00;
			back_jobmode_set.IPYMB1datecode=0x00;  //地址掩码数据码修正
			back_jobmode_set.IPYMB2datecode=0x00;
			back_jobmode_set.IPYMB3datecode=0x00;
			back_jobmode_set.IPYMB4datecode=0x00;
			back_jobmode_set.WGDZB1datecode=0x00;  //网关地址数据码修正
			back_jobmode_set.WGDZB2datecode=0x00;
			back_jobmode_set.WGDZB3datecode=0x00;
			back_jobmode_set.WGDZB4datecode=0x00;
			back_jobmode_set.HopRate=0;      
			back_jobmode_set.SynSignal=0;    
			back_jobmode_set.LinkQuality=0;	
			back_jobmode_set.FaultSignal=0;	
			back_jobmode_set.Silent=0;		
			back_jobmode_set.AllSlient=0;		
			back_jobmode_set.ChannelInfo=0;  
			back_jobmode_set.ChannelTemp=0;  
			back_jobmode_set.ChannelVolt=0;	
			back_jobmode_set.ChannelElect=0;
			back_jobmode_set.RFInfo=0;       
			back_jobmode_set.RFTemp=0;       
			back_jobmode_set.RFVolt=0;		
			back_jobmode_set.RFElect=0;		
			back_jobmode_set.BasedInfo=0;    
			back_jobmode_set.BasedTemp=0;    
			back_jobmode_set.BasedVolt=0;		
			back_jobmode_set.BasedElect=0;	
			back_jobmode_set.Power1Info=0;   
			back_jobmode_set.Power1Temp=0;   
			back_jobmode_set.Power1Volt=0;	
			back_jobmode_set.Power1Elect=0;	
			back_jobmode_set.Power2Info=0;   
			back_jobmode_set.Power2Temp=0;   
			back_jobmode_set.Power2Volt=0;	
			back_jobmode_set.Power2Elect=0;	
			back_jobmode_set.BandwidthSet=0;
			back_jobmode_set.PowerSet=0;		
			back_jobmode_set.Encryption=0;	
			back_jobmode_set.WorkMode=0;		
			back_jobmode_set.FixedFrequency=0;
			back_jobmode_set.AdaHopMinFre=0; 
			back_jobmode_set.AdaHopMaxFre=0; 
			back_jobmode_set.NotAdaHopFre=0; 
			back_jobmode_set.ComNetName=0;   
			back_jobmode_set.MinFreThreshold=0;
			back_jobmode_set.MaxFreThreshold=0;
			back_jobmode_set.Modulation=0;   
			back_jobmode_set.OnlineNodeSum=0;
			back_jobmode_set.ComDataSum=0;   
			back_jobmode_set.ComDataBER=0;   
			back_jobmode_set.ComDataPLP=0;   
			back_jobmode_set.NodeConnect1=0; 
			back_jobmode_set.NodeConnect2=0;
			back_jobmode_set.NodeConnect3=0;
			back_jobmode_set.NodeConnect4=0;
			back_jobmode_set.NodeConnect5=0;
			back_jobmode_set.NodeConnect6=0;
			back_jobmode_set.NodeConnect7=0;
			back_jobmode_set.NodeConnect8=0;
			back_jobmode_set.NodeConnect9=0;
			back_jobmode_set.NodeConnect10=0;
			back_jobmode_set.NodeConnect11=0;
			back_jobmode_set.NodeConnect12=0;
			back_jobmode_set.NodeConnect13=0;
			back_jobmode_set.NodeConnect14=0;
			back_jobmode_set.NodeConnect15=0;
			back_jobmode_set.NodeConnect16=0;
			back_jobmode_set.NodeConnect17=0;
			back_jobmode_set.NodeConnect18=0;
			back_jobmode_set.NodeConnect19=0;
			back_jobmode_set.NodeConnect20=0;
			back_jobmode_set.NodeConnect21=0;
			back_jobmode_set.NodeConnect22=0;
			back_jobmode_set.NodeConnect23=0;
			back_jobmode_set.NodeConnect24=0;
			back_jobmode_set.NodeConnect25=0;
			back_jobmode_set.NodeConnect26=0;
			back_jobmode_set.NodeConnect27=0;
			back_jobmode_set.NodeConnect28=0;
			back_jobmode_set.NodeConnect29=0;
			back_jobmode_set.NodeConnect30=0;
			back_jobmode_set.NodeConnect31=0;
			back_jobmode_set.NodeConnect32=0;
			back_jobmode_set.NodeConnect33=0;
			back_jobmode_set.NodeConnect34=0;
			back_jobmode_set.NodeConnect35=0;
			back_jobmode_set.NodeConnect36=0;
			back_jobmode_set.NodeConnect37=0;
			back_jobmode_set.NodeConnect38=0;
			back_jobmode_set.NodeConnect39=0;
			back_jobmode_set.NodeConnect40=0;
			back_jobmode_set.NodeConnect41=0;
			back_jobmode_set.NodeConnect42=0;
			back_jobmode_set.NodeConnect43=0;
			back_jobmode_set.NodeConnect44=0;
			back_jobmode_set.NodeConnect45=0;
			back_jobmode_set.NodeConnect46=0;
			back_jobmode_set.NodeConnect47=0;
			back_jobmode_set.NodeConnect48=0;
			back_jobmode_set.NodeConnect49=0;
			back_jobmode_set.NodeConnect50=0;
			back_jobmode_set.NodeConnect51=0;
			back_jobmode_set.NodeConnect52=0;
			back_jobmode_set.NodeConnect53=0;
			back_jobmode_set.NodeConnect54=0;
			back_jobmode_set.NodeConnect55=0;
			back_jobmode_set.NodeConnect56=0;
			back_jobmode_set.NodeConnect57=0;
			back_jobmode_set.NodeConnect58=0;
			back_jobmode_set.NodeConnect59=0;
			back_jobmode_set.NodeConnect60=0;
			back_jobmode_set.NodeConnect61=0;
			back_jobmode_set.NodeConnect62=0;
			back_jobmode_set.frameEnd=xorChecksum(&back_jobmode_set,sizeof(struct backjobmodeQuery));
			break;
		}
		case 4:                                                 //构建版本查询回执帧
		{
			version_report.WEBVersion=0x01;                    //WEB版本数据码 ==01
			version_report.MCVersion=0x01;                    //MC版本数据码 ==01
			version_report.NETVersion=0x01;                   //NET版本数据码 ==01
			version_report.SPCLVersion=0x01;
			version_report.JDCLVersion=0x01;
			version_report.frameEnd=xorChecksum(&version_report,sizeof(struct versionReport));
			break；
		}
		default : makeclasses = 0;    
	}
}


/******** 函数名称：recJobmodeQuery（存在问题：工作模式指令尚未定义）********
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web工作参数查询udp指令处理
**********************************************************************/
void recJobmodeQuery(void)
{                                                                  //判定是否为工作模式查询数据
    if(jobmode_query.frameHead == 0x0D && jobmode_query.frameType == 0X01 && jobmode_query.querycode == 0x02 )
	{
		sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
				
	}
}
/*********************************************************************
* 函数名称：recVersionQuery
* 创建时间：2024年8月23日
* 功能描述：接收web版本查询udp指令处理
**********************************************************************/
void recVersionQuery(void)
{
    if(version_query.frameHead == 0x0D && version_query.frameType == 0X01 && version_query.querycode == 0x01)
	{
		    makeSendData(4);
        printf("shu_ju_yi_gou_jian\n");
			sendto(sockfd,&version_report,sizeof(struct versionReport),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		
	}
}
/*************************函数名称：recWLCSSet*************************
* 版本标识：v3.00
* 创建时间：2024年8月23日
* 功能描述：接收web网络参数配置udp指令处理
**********************************************************************/
void recWLCSSet(void)
{
    if(WLCS_set.frameHead == 0x25 && WLCS_set.frameType == 0X0C && WLCS_set.frameCnt == 0x18)
	{
		    makeSendData(3);
			sendto(sockfd,&back_jobmode_set,sizeof(struct backjobmodeQuery),0,(struct sockaddr *)&dest_addr,sizeof(dest_addr));
		 	makeSendData(1);   
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
	unsigned char rx_udp_qurey;
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
	    //int m = 4;
        //GoalId = buffer[m];  //buffer[4]表示数据目的地址号，主控为b
	    //int s = 6;
        //SourceID = buffer[s];//buffer[6]表示数据来源地址号，web网管为c
		if(rx_udp_cnt == 14 &&  rx_udp_type == 0x01)
		{
			rx_udp_qurey = buffer[j+2];
			if(rx_udp_qurey == 2)
			{
				printf("recive jobmodeQuery \n");
				memcpy(&jobmode_query,buffer,rx_udp_cnt);//缓存web工作参数查询数据
				recJobmodeQuery();                       //处理指令
				rx_udp_type = 0;
			}
			else if(rx_udp_qurey == 1)
			{
				printf("recive versionQuery \n");
				memcpy(&version_query,buffer,rx_udp_cnt);//缓存web版本查询指令数据
				recVersionQuery();                       //处理指令
				rx_udp_type = 0;                         // 重置rx_udp_type
			}				// 重置rx_udp_type
	    }else if(rx_udp_cnt == 37  &&  rx_udp_type == 0x0C)
	    {
            printf("recive WLCSSet \n");
	    	memcpy(&WLCS_set,buffer,rx_udp_cnt);     //缓存web网络参数配置指令数据
	    	recWLCSSet();                            //处理指令
            rx_udp_type = 0;                         //重置rx_udp_type
	    }
	    else
	    {
            printf("????date???? \n");
	    	rx_udp_cnt = 0;
	    	memset(&buffer,0, sizeof(buffer));
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