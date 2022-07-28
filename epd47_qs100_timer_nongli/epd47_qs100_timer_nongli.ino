#include <HardwareSerial.h>
#include "RTClib.h"
#include "hz3500_36.h"
#include "hz3500_16.h"

#include <ArduinoJson.h>
//memo缓存管理
#include "memo_historyManager.h"

//https 还未调试成功
//调用会重启！
//编译文件大小 1.1M
HardwareSerial mySerial(1);
RTC_Millis rtc;

//墨水屏缓存区
uint8_t *framebuffer;
memo_historyManager* objmemo_historyManager;


const int short_time_segment = 60;  //休眠唤醒最小分钟时间间隔
uint32_t TIME_TO_SLEEP = 3600; //下次唤醒间隔时间(3600秒）

bool net_connect_succ = false;

int starttime, stoptime;

String nongli_data_table = ""; //农历json字符串

bool state_sync_time = false;
bool  state_sync_nongli = false;

String http_nongli_host = "http://api.tianapi.com";
//建议自已申请key 都用这个key容易并发限制
//https://www.tianapi.com/
String http_nongli_url = "/lunar/index?key=f3e86cd4281bf1ae71c933e8b38a407c&date=";

int cnt_check_net = 0;
int cnt_sync_nongli  = 0;

char daysOfTheWeek[7][12] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */
hw_timer_t *timer = NULL;

String buff_split[20];

/*
  通过esp32墨水屏+ qt100(nbiot模块)
  每天1点左右同步日历
  平均用时时间：70秒
  3分钟如果不执行完全部操作会自动重启,防止因为程序跑飞耗尽电池

  电流：50ma
  编译大小: 2.3M
  开发板选择: TTGO-W-WATCH (仅用到分区定义，原因为汉字库较大)

  偶尔发现usb供电时，qs100不供电，原因是qs100会自动休眠，如果首次上电才能用
*/

void IRAM_ATTR resetModule() {
  ets_printf("resetModule reboot\n");
  delay(100);
  //esp_restart_noos(); 旧api
  esp_restart();
}


//文字显示
void Show_hz(String rec_text, bool loadbutton)
{
  //最长限制160字节，40汉字
  //6个字串，最长约在 960字节，小于1024, json字串最大不超过1024
  rec_text = rec_text.substring(0, 160);
  Serial.println("begin Showhz:" + rec_text);


  epd_poweron();
  //uint32_t t1 = millis();
  //全局刷
  epd_clear();
  //局刷,一样闪屏
  //epd_clear_area(screen_area);
  //epd_full_screen()

  //此句不要缺少，否则显示会乱码
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  //特殊标志符
  if (rec_text != "[clear]")
  {
    //更正汉字符号显示的bug
    rec_text.replace("，", ",");
    rec_text.replace("。", ".");
    rec_text.replace("？", "?");


    //uint32_t t2 = millis();
    //printf("EPD clear took %dms.\n", t2 - t1);
    int cursor_x = 10;
    int cursor_y = 80;

    //多行文本换行显示算法。
    if (!loadbutton)
      objmemo_historyManager->multi_append_txt_list(rec_text);

    String now_string = "";
    int i;
    //write_string 能根据手工加的 "\n"换行，但不能自由控制行距，此处我自行控制了.
    for ( i = 0; i < objmemo_historyManager->memolist.size() ; i++)
    {
      now_string = objmemo_historyManager->memolist.get(i);
      //Serial.println("Show_hz line:" + String((now_index + i) % TXT_LIST_NUM) + " " + now_string);

      if (now_string.length() > 0)
      {
        //加">"字符，规避epd47的bug,当所有字库不在字库时，esp32会异常重启
        // “Guru Meditation Error: Core 1 panic'ed (LoadProhibited). Exception was unhandled."
        now_string = ">" + now_string;
        //墨水屏writeln不支持自动换行
        //delay(200);
        //一定要用framebuffer参数，否则当最后一行数据过长时，会导致代码在此句阻塞，无法休眠，原因不明！

        writeln((GFXfont *)&msyh36, (char *)now_string.c_str(), &cursor_x, &cursor_y, framebuffer);
        //writeln调用后，cursor_x会改变，需要重新赋值
        cursor_x = 10;
        cursor_y = cursor_y + 85;
      }
    }
  }
  //前面不要用writeln，有一定机率阻塞，无法休眠
  epd_draw_grayscale_image(epd_full_screen(), framebuffer);

  //delay(500);
  epd_poweroff();

  //清空显示
  objmemo_historyManager->memolist.clear();
  Serial.println("end Showhz:" + rec_text );
}

//毫秒内不接收串口数据，并清缓存
void clear_uart(int ms_time)
{
  //唤醒完成后就可以正常接收串口数据了
  uint32_t starttime = 0;
  char ch;
  //5秒内有输入则输出
  starttime = millis();
  //临时接收缓存，防止无限等待
  while (true)
  {
    if  (millis()  - starttime > ms_time)
      break;
    while (mySerial.available())
    {
      ch = (char) mySerial.read();
      Serial.print(ch);
    }
    yield();
    delay(20);
  }
}


bool get_nongli()
{
  bool succ_flag = false;

  String ret;
  cnt_sync_nongli = cnt_sync_nongli + 1;


  free_http();
  delay(1000);

  nongli_data_table = "";

  //注意： qs-100 最后不需要 "/"
  ret = send_at("AT+HTTPCREATE=\"" + http_nongli_host + "\"", "OK", 3);
  Serial.println("ret=" + ret);
  if (not (ret.indexOf("+HTTPCREATE:0") > -1))
    return false;

  Serial.println(">>> 创建HTTP Host ok ...");
  delay(2000);

  Serial.println(">>> 获取数据 ...");
  //最长120秒内获得数据
  ret = send_at_httpget("AT+HTTPSEND=0,0,\"" + http_nongli_url + Get_softrtc_time(7) + "\"", 30);
  clear_uart(1000);
  Serial.println("ret=" + ret);
  Serial.println("ret len=" + String(ret.length()));
  if  (ret.length() > 500 && ret.indexOf("success") > -1)
  {
    nongli_data_table = ret;
    Serial.println("nongli=" + nongli_data_table);

    Serial.println("deserializeJson");
    //字符长度约1020B上下，为保险，3KB，
    //必须用 DynamicJsonDocument
    DynamicJsonDocument  root(5 * 1024);
    deserializeJson(root, nongli_data_table);

    //节目
    String jieri = root["newslist"][0]["lunar_festival"].as<String>();

    if (root["newslist"][0]["festival"].as<String>().length() > 0)
      jieri = jieri + " " + root["newslist"][0]["festival"].as<String>();

    if (root["newslist"][0]["jieqi"].as<String>().length() > 0)
      jieri = jieri + " " + root["newslist"][0]["jieqi"].as<String>();


    String show_txt = Get_softrtc_time(8)  +  "\n" +
                      root["newslist"][0]["lunardate"].as<String>().substring(0, 4) + "年" + root["newslist"][0]["lubarmonth"].as<String>() + root["newslist"][0]["lunarday"].as<String>() + "\n" ;

    if (jieri.length() > 0)
      show_txt = show_txt + jieri + "\n" ;
    show_txt = show_txt +   "宜:" + root["newslist"][0]["fitness"].as<String>()  + "\n"  +
               "忌:" + root["newslist"][0]["taboo"].as<String>() ;

    Show_hz(show_txt, false);

    cnt_sync_nongli = 0;
    succ_flag = true;

  }
  else
    Serial.println("无效的返回数据");
  delay(500);
  return succ_flag;
}

//readStringUntil 注意：如果一直等不到结束符会阻塞
String send_at2(String p_char, String break_str, String break_str2, int delay_sec) {

  String ret_str = "";
  String tmp_str = "";
  if (p_char.length() > 0)
  {
    Serial.println(String("cmd=") + p_char);
    mySerial.println(p_char);
  }

  //发完命令立即退出
  //if (break_str=="") return "";

  mySerial.setTimeout(1000);

  uint32_t start_time = millis() / 1000;
  while (millis() / 1000 - start_time < delay_sec)
  {
    if (mySerial.available() > 0)
    {

      tmp_str = mySerial.readStringUntil('\n');
      //tmp_str.replace("\r", "");
      tmp_str = tmp_str.substring(0, tmp_str.length() - 1); //去掉串尾的 \r
      //tmp_str.trim()  ;
      Serial.println(">" + tmp_str);
      //如果字符中有特殊字符，用 ret_str=ret_str+tmp_str会出现古怪问题，最好用concat函数
      ret_str.concat(tmp_str);
      if (break_str.length() > 0 && tmp_str.indexOf(break_str) > -1 )
        break;
      if (break_str2.length() > 0 &&  tmp_str.indexOf(break_str2) > -1 )
        break;
    }
    delay(10);
  }
  return ret_str;
}


//readStringUntil 注意：如果一直等不到结束符会阻塞
String send_at(String p_char, String break_str, int delay_sec) {

  String ret_str = "";
  String tmp_str = "";
  if (p_char.length() > 0)
  {
    Serial.println(String("cmd=") + p_char);
    mySerial.println(p_char);
  }

  //发完命令立即退出
  //if (break_str=="") return "";

  mySerial.setTimeout(1000);

  uint32_t start_time = millis() / 1000;
  while (millis() / 1000 - start_time < delay_sec)
  {
    if (mySerial.available() > 0)
    {
      tmp_str = mySerial.readStringUntil('\n');
      //tmp_str.replace("\r", "");
      tmp_str = tmp_str.substring(0, tmp_str.length() - 1); //去掉串尾的 \r
      //tmp_str.trim()  ;
      Serial.println(">" + tmp_str);
      //如果字符中有特殊字符，用 ret_str=ret_str+tmp_str会出现古怪问题，最好用concat函数
      ret_str.concat(tmp_str);
      if (break_str.length() > 0 && tmp_str.indexOf(break_str) > -1)
        break;
    }
    delay(10);
  }
  return ret_str;
}

//检查是否关机状态
bool check_waker_7020()
{
  String ret = "";
  delay(1000);
  int cnt = 0;
  bool check_ok = false;
  //通过AT命令检查是否关机，共检查3次
  while (true)
  {
    cnt++;

    //不要用 send_at("AT", "", 2);
    //如果一直收不到 \n 字串，程序会阻塞
    ret = send_at("AT", "", 2);
    Serial.println("ret=" + ret);
    if (ret.indexOf("OK") > -1)
    {
      check_ok = true;
      break;
    }
    if (cnt >= 5) break;
    delay(1000);
  }
  return check_ok;
}




bool connect_nb()
{
  bool  ret_bool = false;

  int error_cnt = 0;
  String ret;

  cnt_check_net = cnt_check_net + 1;
  Serial.println(">>> 检查网络连接状态 ...");
  error_cnt = 0;
  //网络注册状态
  while (true)
  {
    ret = send_at("AT+CEREG?", "OK", 3);
    Serial.println("ret=" + ret);
    if (ret.indexOf("+CEREG:0,1") > -1)
      break;
    delay(2000);

    error_cnt++;
    if (error_cnt >= 5)
      return false;
  }
  Serial.println(">>> 网络注册状态 ok ...");
  delay(1000);


  error_cnt = 0;
  //查询附着状态
  while (true)
  {
    ret = send_at("AT+CGATT?", "OK", 3);
    Serial.println("ret=" + ret);

    if (ret.indexOf("+CGATT:1") > -1)
      break;
    delay(2000);

    error_cnt++;
    if (error_cnt >= 5)
      return false;
  }
  Serial.println(">>> 附着状态 ok ...");

  delay(1000);

  error_cnt = 0;
  //同步时间
  while (true)
  {
    ret = send_at("AT+CCLK?", "OK", 3);
    Serial.println("ret=" + ret);

    if (ret.indexOf("+CCLK:") > -1)
      break;
    delay(2000);

    error_cnt++;
    if (error_cnt >= 5)
      return false;
  }

  Serial.println(">>> 获取时间成功 ...");
  //+CCLK:22/07/15,13:36:45+32OK
  state_sync_time = false;
  if (ret.startsWith("+CCLK:") && ret.length() > 10 )
  {
    state_sync_time = true;
    ret.replace("OK", "");
    Serial.println("获取时间:" + ret);
    sync_esp32_rtc(ret);
  }

  cnt_check_net = 0;
  return true;
}

//把 +CCLK:22/07/14,14:50:41+32 转换成esp32内的时间
void sync_esp32_rtc(String now_time)
{
  now_time.replace("+CCLK:", "##");
  DateTime now = DateTime(now_time.substring(2, 4).toInt() + 2000, now_time.substring(5, 7).toInt(), now_time.substring(8, 10).toInt(),
                          now_time.substring(11, 13).toInt(), now_time.substring(14, 16).toInt(), now_time.substring(17, 19).toInt());
  // calculate a date which is 7 days and 30 seconds into the future
  //增加8小时
  DateTime future (now.unixtime() + 28800L);
  rtc.adjust(future);
  Serial.println("now_time:" + Get_softrtc_time(6));
}


void splitString(String message, String dot, String outmsg[], int len)
{
  int commaPosition, outindex = 0;
  for (int loop1 = 0; loop1 < len; loop1++)
    outmsg[loop1] = "";
  do {
    commaPosition = message.indexOf(dot);
    if (commaPosition != -1)
    {
      outmsg[outindex] = message.substring(0, commaPosition);
      outindex = outindex + 1;
      message = message.substring(commaPosition + 1, message.length());
    }
    if (outindex >= len) break;
  }
  while (commaPosition >= 0);

  if (outindex < len)
    outmsg[outindex] = message;
}

int parse_CHTTPNMIC(String in_str)
{
  //+HTTPNMIC:0,1,5231,815
  String out_str = "";
  int cnt = 0;
  splitString(in_str, ",", buff_split, 4);
  cnt = buff_split[3].toInt() ;
  return cnt;
}


String send_at_httpget(String p_char, int delay_sec) {
  String ret_str = "";
  String tmp_str = "";
  if (p_char.length() > 0)
  {
    Serial.println(String("cmd=") + p_char);
    mySerial.println(p_char);
  }
  ret_str = "";
  mySerial.setTimeout(5000);
  uint32_t start_time = millis() / 1000;
  int content_len;
  while (millis() / 1000 - start_time < delay_sec)
  {

    if (mySerial.available() > 0)
    {
      tmp_str = mySerial.readStringUntil('\n');
      tmp_str = tmp_str.substring(0, tmp_str.length() - 1); //去掉串尾的 \r

      Serial.println(tmp_str);
      //数据接收完成，正常退出
      if (tmp_str.startsWith("+HTTPDICONN:0,-2"))
      {
        Serial.println("数据接收完毕,break");
        break;
      }

      if (tmp_str.startsWith("+REQUESTSUCCESS") )
      {
        Serial.println("开始数据接收...");
        //break;
      }

      //没有获得数据
      if (tmp_str.startsWith("+BADREQUEST"))
      {
        Serial.println("未获得数据,break");
        break;
      }

      if (tmp_str.startsWith("+HTTPNMIC:0,"))
      {
        content_len = parse_CHTTPNMIC(tmp_str);
        tmp_str = mySerial.readStringUntil('\n');
        tmp_str = tmp_str.substring(0, tmp_str.length() - 1); //去掉最后的/r
        Serial.println(">" + tmp_str);
        Serial.println("content_len=" + String(content_len));
        Serial.println("str_len=" + String(tmp_str.length()));
        ret_str = ret_str + tmp_str;
      }

    }
    delay(10);
  }

  return ret_str;
}


void free_http()
{
  String ret;
  ret = send_at2("AT+HTTPCLOSE=0", "OK", "ERROR" , 5);
  Serial.println("ret=" + ret);
  Serial.println(">>> 断开http连接  ok ...");
}



String Get_softrtc_time(int flag)
{
  if (state_sync_time == false)
    return "";

  DateTime now = rtc.now();
  char buf[50];
  buf[0] = '\0';
  if (flag == 0)
  {
    sprintf(buf, "%02d,%02d,%02d,%02d,%02d,%02d", now.year(), now.month() , now.day(), now.hour(), now.minute(), now.second());
  }
  if (flag == 1)
  {
    sprintf(buf, "%02d:%02d", now.hour(), now.minute());
  }
  else if (flag == 2)
  {
    sprintf(buf, "%02d,%02d,%02d,%02d,%02d", now.year(), now.month() , now.day(), now.hour(), now.minute());

  }
  else if (flag == 3)
  {
    sprintf(buf, "%02d月%02d日%s",  now.month() , now.day(), daysOfTheWeek[now.dayOfTheWeek()]);
  }
  else if (flag == 4)
  {
    sprintf(buf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  }
  else if (flag == 5)
  {
    sprintf(buf, "%02d-%02d-%02d %02d:%02d", now.year(), now.month() , now.day(), now.hour(), now.minute());
  }
  else if (flag == 6)
  {
    sprintf(buf, "%02d-%02d-%02d %02d:%02d:%02d", now.year(), now.month() , now.day(), now.hour(), now.minute(), now.second());
  }
  else if (flag == 7)
  {
    sprintf(buf, "%04d-%02d-%02d", now.year(), now.month() , now.day());
  }
  else if (flag == 8)
  {
    sprintf(buf, "%04d年%02d月%02d日 %s", now.year() , now.month() , now.day(), daysOfTheWeek[now.dayOfTheWeek()]);
  }
  return String(buf);
}


//每小时一次
void goto_sleep_test()
{
  Serial.println("goto sleep");

  //如果前次有成功同步时间
  if (state_sync_time)
  {
    DateTime now = rtc.now();
    String now_time = Get_softrtc_time(1);
    Serial.println("now_time:" + now_time);

    //计算本次需要休眠秒数

    Serial.println("计算休眠时间 hour=" + String(now.hour() ) + ",minute=" + String(now.minute() ) + ",second=" + String( now.second()));

    //如果short_time_segment是1，表示每1分钟整唤醒一次,定义的闹钟时间可随意
    //                       5，表示每5分钟整唤醒一次,这时定义的闹钟时间要是5的倍数，否则不会定时响铃
    TIME_TO_SLEEP = (short_time_segment - now.minute() % short_time_segment) * 60;
    TIME_TO_SLEEP = TIME_TO_SLEEP - now.second();

    //休眠时间过短，低于30秒直接视同0
    if (TIME_TO_SLEEP < 30)
      TIME_TO_SLEEP = 60 * short_time_segment + 50;

    //考虑到时钟误差，增加几秒，确保唤醒时间超出约定时间
    //TIME_TO_SLEEP = TIME_TO_SLEEP +10;


    Serial.println("go sleep,wake after " + String(TIME_TO_SLEEP)  + "秒 AT:" + Get_softrtc_time(0));
  }
  //时间未同步，时间无效，1小时后再试
  else
  {
    TIME_TO_SLEEP = 3600 + 10;
    Serial.println("时间未同步， go sleep,wake after " + String(TIME_TO_SLEEP)  + "秒 ");
  }

  stoptime = millis() / 1000;

  //平均35-80秒不等，nbiot同步时间需要时间较长
  Serial.println("wake 用时:" + String(stoptime - starttime) + "秒");

  Serial.flush();

  //It will turn off the power of the entire
  // POWER_EN control and also turn off the blue LED light
  epd_poweroff_all();

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);

  // ESP进入deepSleep状态
  //最大时间间隔为 4,294,967,295 µs 约合71分钟
  //休眠后，GPIP的高，低状态将失效，无法用GPIO控制开关
  esp_deep_sleep_start();
}

//计算总休眠秒数 到TIME_TO_SLEEP
void cal_waker_seconds()
{
  //如果前次有成功同步时间
  if (state_sync_time)
  {
    DateTime now = rtc.now();
    String now_time = Get_softrtc_time(1);
    Serial.println("now_time:" + now_time);

    //计算本次需要休眠秒数

    Serial.println("计算休眠时间 hour=" + String(now.hour() ) + ",minute=" + String(now.minute() ) + ",second=" + String( now.second()));

    //先计算到 00:00的秒数
    TIME_TO_SLEEP =   ((24 - now.hour()) * 60 -  now.minute() ) * 60;
    TIME_TO_SLEEP = TIME_TO_SLEEP - now.second();

    if (TIME_TO_SLEEP < 30)
      TIME_TO_SLEEP = 24 * 60 * 60 + 50;

    //再加上到 01:00的秒数
    TIME_TO_SLEEP = TIME_TO_SLEEP + 1 * 3600;

    //24小时唤醒平均会少15-20分钟，所以用15分钟当误差
    TIME_TO_SLEEP = TIME_TO_SLEEP + 15 * 60 ;
    Serial.println("go sleep,wake after " + String(TIME_TO_SLEEP)  + "秒 AT:" + Get_softrtc_time(0));

  }
  //时间未同步，时间无效，2小时后再试
  else
  {
    TIME_TO_SLEEP = 3600 * 2 + 10;
    Serial.println("时间未同步， go sleep,wake after " + String(TIME_TO_SLEEP)  + "秒 ");
  }
}


//每天1次,早7点整
void goto_sleep()
{
  Serial.println("goto sleep");

  //计算休眠秒数
  cal_waker_seconds();

  stoptime = millis() / 1000;

  //平均35-80秒不等，nbiot同步时间需要时间较长
  Serial.println("wake 用时:" + String(stoptime - starttime) + "秒");

  Serial.flush();

  //It will turn off the power of the entire
  // POWER_EN control and also turn off the blue LED light
  epd_poweroff_all();

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);

  // ESP进入deepSleep状态
  //最大时间间隔为 4,294,967,295 µs 约合71分钟
  //休眠后，GPIP的高，低状态将失效，无法用GPIO控制开关
  esp_deep_sleep_start();
}


//GetCharwidth函数本来应放在 memo_historyManager类内部
//但因为引用了 msyh24海量字库变量，会造成编译失败,所以使用了一些技巧
//将函数当指针供类memo_historyManager 使用
//计算墨水屏显示的单个字的长度
int GetCharwidth(String ch)
{
  //修正，空格计算的的宽度为0, 强制36 字体不一样可能需要修改！
  if (ch == " ") return 28;

  char buf[50];
  int x1 = 0, y1 = 0, w = 0, h = 0;
  int tmp_cur_x = 0;
  int tmp_cur_y = 0;
  FontProperties properties;
  get_text_bounds((GFXfont *)&msyh36, (char *) ch.c_str(), &tmp_cur_x, &tmp_cur_y, &x1, &y1, &w, &h, &properties);
  //sprintf(buf, "x1=%d,y1=%d,w=%d,h=%d", x1, y1, w, h);
  //Serial.println("ch="+ ch + ","+ buf);

  //负数说明没找到这个字,会不显示出来
  if (w <= 0)
    w = 0;
  return (w);
}

void setup() {

  starttime = millis() / 1000;

  Serial.begin(115200);
  //                               RX  TX
  mySerial.begin(9600, SERIAL_8N1, 12, 13);

  //00/01/01,00:00:01+32
  DateTime now = DateTime(2000, 01, 01, 00, 00, 00);
  rtc.adjust(now);

  //如果启动后不调用此函数，有可能电流一直保持在在60ma，起不到节能效果
  //此步骤不适合在唤醒后没有显示需求时优化掉
  epd_init();
  // framebuffer = (uint8_t *)heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM);
  framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
  if (!framebuffer) {
    Serial.println("alloc memory failed !!!");
    delay(1000);
    while (1);
  }
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  objmemo_historyManager = new memo_historyManager();
  objmemo_historyManager->GetCharwidth = GetCharwidth;

  //建议时间 5秒
  delay(5000);  //等待一会，确保网络连接上
  Serial.println(">>> 开启 nb-iot ...");
  clear_uart(5000);

  //为防意外，n秒后强制复位重启，一般用不到。。。
  //n秒如果任务处理不完，看门狗会让esp32自动重启,防止程序跑死...
  uint32_t wdtTimeout = 3 * 60 * 1000; //设置3分钟 watchdog
  timer = timerBegin(0, 80, true);                  //timer 0, div 80
  timerAttachInterrupt(timer, &resetModule, true);  //attach callback
  timerAlarmWrite(timer, wdtTimeout * 1000 , false); //set time in us
  timerAlarmEnable(timer);                          //enable interrupt

  Serial.println("check_waker_7020");

  //at预处理
  check_waker_7020();

  net_connect_succ = false;

  /*
    {"code":200,"msg":"success","newslist":[{"gregoriandate":"2022-07-28","lunardate":"2022-6-30","lunar_festival":"","festival":"","fitness":"祭祀.交易.收财.安葬","taboo":"宴会.安床.出行.嫁娶.移徙","shenwei":"喜神：正南 福神：东南 财神：正南阳贵：正东 阴贵：东南 ","taishen":"仓库忌修弄,碓须忌,厨灶莫相干胎神在外西北停留6天","chongsha":"马日冲(丙子)鼠","suisha":"岁煞北","wuxingjiazi":"金","wuxingnayear":"金箔金","wuxingnamonth":"天河水","xingsu":"东方角木蛟-吉","pengzu":"壬不泱水 午不苫盖","jianshen":"闭","tiangandizhiyear":"壬寅","tiangandizhimonth":"丁未","tiangandizhiday":"壬午","
  */
  //String test_nongli = "{\"results\":[{\"location\":{\"id\":\"WX4FBXXFKE4F\",\"name\":\"北京\",\"country\":\"CN\",\"path\":\"北京,北京,中国\",\"timezone\":\"Asia/Shanghai\",\"timezone_offset\":\"+08:00\"},\"daily\":[{\"date\":\"2022-06-17\",\"text_day\":\"小雨\",\"code_day\":\"13\",\"text_night\":\"多云\",\"code_night\":\"4\",\"high\":\"31\",\"low\":\"20\",\"rainfall\":\"5.40\",\"precip\":\"0.98\",\"wind_direction\":\"东南\",\"wind_direction_degree\":\"135\",\"wind_speed\":\"8.4\",\"wind_scale\":\"2\",\"humidity\":\"74\"},{\"date\":\"2022-06-18\",\"text_day\":\"多云\",\"code_day\":\"4\",\"text_night\":\"晴\",\"code_night\":\"1\",\"high\":\"33\",\"low\":\"22\",\"rainfall\":\"0.00\",\"precip\":\"0.00\",\"wind_direction\":\"东北\",\"wind_direction_degree\":\"45\",\"wind_speed\":\"8.4\",\"wind_scale\":\"2\",\"humidity\":\"74\"},{\"date\":\"2022-06-19\",\"text_day\":\"晴\",\"code_day\":\"0\",\"text_night\":\"晴\",\"code_night\":\"1\",\"high\":\"34\",\"low\":\"22\",\"rainfall\":\"0.00\",\"precip\":\"0.00\",\"wind_direction\":\"东南\",\"wind_direction_degree\":\"135\",\"wind_speed\":\"3.0\",\"wind_scale\":\"1\",\"humidity\":\"77\"}],\"last_update\":\"2022-06-17T08:00:00+08:00\"}]}";
  //Serial.println("draw_nongli:" + test_nongli);
  //draw_nongli(test_weather);

  Serial.println("setup");
}



void loop() {

  //3次失败检测网络连接
  if (cnt_check_net >= 3)
    goto_sleep();

  //3次失败获取农历
  if (cnt_sync_nongli >= 3)
    goto_sleep();

  //农历已同步
  if (state_sync_nongli)
    goto_sleep();

  //上电已超过 2分钟
  //实测70秒足够
  stoptime = millis() / 1000;
  if (stoptime - starttime >= 2 * 60)
    goto_sleep();

  //如果setup时网络连接失败，重新再试
  if (net_connect_succ == false)
  {
    clear_uart(5000);
    Serial.println(">>> 检查网络连接 ...");
    net_connect_succ = connect_nb();
    return;
  }

  if (state_sync_nongli == false)
  {
    clear_uart(5000);
    //实测连上网后，时针一般都同步上了，不需要判断
    state_sync_nongli = get_nongli();
    return;
  }

  delay(1000);
}
