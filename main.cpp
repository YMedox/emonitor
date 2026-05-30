/*--------------------------------------------------------------------------------------------------------------------------------------
Сбор параметров электроснабжения по Modbus и отправка их в OpenHAB.
Использованиа библиотека libmodbus, https://libmodbus.org
В основном потоке измеряются основные параметры, в дополнительном потоке отправляются данные в OpenHAB.
Программа поддерживает два типа устройств: PZEM016 и 8-канальный 

Автор - Ярослав Медокс
--------------------------------------------------------------------------------------------------------------------------------------*/
#include <string>
#include <unistd.h>
#include <modbus.h>
#include <curl/curl.h>
#include <thread>
#include <mutex>
#include <atomic>


#include "iniobject.hpp"
#include "mlogger.hpp"
#include "mtimer.hpp"
#include "version.h"

// Логгер
cLogger *logger;
#define logmessage logger->log
#define logperror(x) logger->log(ERROR, (char*)"In %s() line %d. %s: %s", __FUNCTION__,  __LINE__, x, strerror(errno))
#define logcppinfo *logger<<INFO
#define logcppwarn *logger<<WARN
#define logcpperror *logger<<ERROR<<"In "<<__FUNCTION__<<"() line "<<__LINE__<<". "
#define logcppdebug *logger<<DEBUG<<"In "<<__FUNCTION__<<"() line "<<__LINE__<<". "

#define MODBUS_USB 1
#define MODBUS_TCP 2

struct {
  char *destination;
  uint8_t type;
  unsigned long poll;
  unsigned long report;
  char *rep_url;
  char *bearer;
  long timeout_ms;
} params;

// Глобальные переменные
std::atomic<bool> bCont{true};
modbus_t *mb;
std::mutex repMutex;
std::string repString;
bool bNewReport = false;

// Если не определена ни одна из констант, выдаст ошибку компиляции.
std::string getVersion() {
  std::string ret = "Version " + std::string(_v_.version) + ". Built on \"" + std::string(_v_.machine) + "\" " + std::string(_v_.date) + ". ";
#ifdef __DEBUG__
  std::string ret1 =  "Debug";
#else
  #ifdef __RELEASE__
    std::string ret1 =  "Release";
  #endif
#endif
  return ret + ret1 + " version.";
}

// Отправка данных в OpenHAB. Функция потока, создаваемого в main()
void sendToOpenHAB() {
  while(bCont.load() == true) {
	CURL *curl;
	CURLcode res = CURLE_OK;
	struct curl_slist *headers = NULL;
    if(params.rep_url == NULL) {
      logcpperror<<"Destiantion URL not set up in conf file"<<ENDL;
      return;
    }
    repMutex.lock();
    if(bNewReport) {    // Отправляем только если есть данные для отправки
	  logcppinfo<<"Sending to OpenHAB: "<<repString<<ENDL;
	  curl = curl_easy_init();
	  if(curl) {
	    curl_easy_setopt(curl, CURLOPT_URL, params.rep_url);
	    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, params.timeout_ms);
	    headers = curl_slist_append(headers, "accept: */*");
	    headers = curl_slist_append(headers, "Content-Type: text/plain");
	    if(params.bearer != NULL) {
  	      std::string bear = "Authorization: Bearer " + std::string(params.bearer);
	      headers = curl_slist_append(headers, bear.c_str());
        }
	     /* post binary data */
	    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, repString.c_str());
	     /* pass our list of custom made headers */
	    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	    /* Perform the request, res will get the return code */
	    res = curl_easy_perform(curl);
	    /* Check for errors */
	    if(res != CURLE_OK)
	      logcpperror<<"curl_easy_perform() failed: "<<curl_easy_strerror(res)<<ENDL;
	    /* free the header list */
	    curl_slist_free_all(headers);
	    /* always cleanup */
	    curl_easy_cleanup(curl);
	  } else {
	      logcpperror<<"curl_easy_init() failed"<<ENDL;
	  }
    }
    bNewReport = false;
    repMutex.unlock();
    usleep(200000L); //0.2 секунды. На самом деле чтение параметров занимает всё время, а отправка очень быстрая
  }
}

// Запуск логирования
void initLogger(cIniObject *ini) {
   if(!logger) {
      cLogParams p;
      char *group  = (char*)"Logger";
      p.max_messages     = ini->getInt(group, (char*)"mmax", 100);
      p.max_size_of_file = ini->getInt(group, (char*)"fsize", 1000000);
      p.path             = ini->getString(group, (char*)"fpath");
      p.num_files        = ini->getInt(group, (char*)"fnum", 3);
      p.sink_cout        = true; 
      p.sink_log         = true;
      p.tune_sleep       = true;  //этот логгер основной и имеет право регулировать скорость.
      p.log_level        = logger_getDebugLevelFromChar(ini->getString(group, (char*)"level"));
      logger = logger_new(&p);
      logger_setSleepTime(ini->getInt(group, (char*)"stime", 10000));  //можно установить только после создания первого логгера
      if(p.path) free(p.path);
      if(!logger) {
        exit(1);
      }
      logcppinfo<<"emonitor logger started with level "<<c_level[p.log_level]<<ENDL;
   }
}

// Базовый класс для Modbus-стройств
class modbusDevice {
  protected:
    uint16_t _addr;
    const std::string delimiter = ";";
    bool bLoaded = false;
  public:
    virtual void poll(bool bNewDate) {};
    bool connect() {
      if (modbus_set_slave(mb, _addr) == -1) {
        logcpperror<<"Invalid slave ID: "<<_addr<<ENDL;
        return false;
      }
      if(modbus_connect(mb) == -1) {
        logcpperror<<"Connection to slave "<<_addr<<" failed: "<<modbus_strerror(errno)<<ENDL;
        return false;
      }
      return true;
    }
    void close() {
      modbus_close(mb);
    }
    virtual void save() { }
    virtual std::string getReportString() { return ""; }
//    ~modbusDevice() { save(); }
};
//Список всех опрашиваемых устройств
std::vector<modbusDevice *> md;

// Класс для измерителя электрических параметров PZEM016, https://peacefair.en.made-in-china.com/product/LACUvOQWCIki/China-Pzem-014-016-Smart-Electric-Energy-Power-Meter-10A-32A-63A-100A-Single-Phase-RS485-Port-Modbus-Kwh-Frequency-AC-Current-Voltage-Meters.html
#define PZEMSIZE 9
class PZEM016 : public modbusDevice {
  private:
    uint16_t tab_reg[PZEMSIZE];
    double U, I, P, PA, F, ED, ET, EO;
  public:
    PZEM016(char *addr) {
      _addr = atoi(addr);
      logcppinfo<<"Added PZEM016 with address "<<addr<<ENDL;
    }
    void load() {
      std::string line;
      char *send;
      std::ifstream saved(std::to_string(_addr));
      if (saved.is_open()) {
        std::getline(saved, line); 
        ED = strtof(line.c_str(), &send);
        logcppinfo<<"PZEM016 with addr "<<_addr<<" loaded data ED="<<ED<<ENDL;
        saved.close();
        EO = ET - ED;   //отсчет заново, хотя можно было бы сохранить смещение. ДЛя единообразия сделано
      } else {
         logcpperror<<"File "<<_addr<<" not opened!"<<ENDL;
      }
      bLoaded = true;
    }
    virtual void save() {   //деструктор не получилось вызвать, поэтому в CleanUp прямой вызов save
      if(!bLoaded) return;
      std::fstream saved(std::to_string(_addr));
      saved<<ED<<std::endl;
      saved.close();
      logcppinfo<<"PZEM016 with addr "<<_addr<<" saved data ED="<<ED<<ENDL;
    }
    void calcAll() {
      U  = double(tab_reg[0]) * 0.1;
      I  = double(tab_reg[1] + tab_reg[2] * 65535) * 0.001;
      P  = double(tab_reg[3] + tab_reg[4] * 65535) * 0.1;
      ET = double(tab_reg[5] + tab_reg[6] * 65535) * 0.001;
      F  = double(tab_reg[7]) * 0.1;
      double factor = double(tab_reg[8]) * 0.01;
      PA =  P / factor;
      ED = ET - EO;      //потреблено за сутки
    }
    virtual void poll(bool bNewDate) {
      logcppinfo<<"Polling addr "<<_addr<<ENDL;
      if(connect()) {
        if(modbus_read_input_registers(mb, 0, PZEMSIZE, tab_reg) == -1) {
          logcpperror<<"Read slave "<<_addr<<" failed: "<<modbus_strerror(errno)<<ENDL;
        }
        close();
        if(bNewDate) {
           calcAll(); //избыточно
           EO = ET;   //устанавливаем смещение
           ED = 0;    //сбрасываем суточный счетчик
           if(bLoaded) save();
           else        { calcAll(); load(); } //нужно получить ET
        }
      }
    }
    std::string addValue(double value) {
      char buf[32];
      sprintf(buf, "%0.1f", value);
      return std::string(buf) + delimiter;
    }
    virtual std::string getReportString() {
      std::string ret = "";
      calcAll();  //нужно вызывать для актуализации параметров и вычисленной энергии
      ret += addValue(U);
      ret += addValue(I);
      ret += addValue(P);
      ret += addValue(PA);
      ret += addValue(F);
      ret += addValue(ED);
      ret += addValue(ET);
      return ret;
    }
};

// Класс для Modbus считывателя аналоговых сигналов с датчиков. 8-канальный модуль захвата данных.
// https://www.waveshare.com/wiki/Modbus_RTU_Analog_Input_8CH?spm=a2g2w.detail.0.1.570e3a8cpKxDwi
#define MBSL8AISIZE 6  //регистров на самом деле 8!!! Используются пока только 6
class MBSL8AI : public modbusDevice {
  private:
    uint16_t tab_reg[MBSL8AISIZE];
    uint16_t offsets[MBSL8AISIZE];
    double   maxs[MBSL8AISIZE];
    double   coeff;
    double   UInp, UBatt, IInp, IOutp, IInv, PInp, POutp, EInp, EOutp;
    suseconds_t last, avrg=0;
  public:
    MBSL8AI(char *addr, cIniObject *ini) {
      _addr = atoi(addr);
      char key[12];
      for(int i=1;i<=MBSL8AISIZE;i++) {
        sprintf(key, "AI%d_offset", i);
        offsets[i-1] = ini->getUInt(addr, key, 819);
        sprintf(key, "AI%d_max", i);
        maxs[i-1] = ini->getDouble(addr, key, 0);
      }
      coeff = double(20) / double(16*4096); //коэффициент пересчета из 0-20ma в 4-20ma
      resetE();
      last = getMillis();
      PInp = POutp = 0;
      logcppinfo<<"Added MBSL8AI with address "<<addr<<ENDL;
    }
    void load() {
      std::string line;
      char *send;
      std::ifstream saved(std::to_string(_addr));
      if (saved.is_open()) {
        std::getline(saved, line);
        EInp = strtod(line.c_str(), &send);  
        std::getline(saved, line);
        EOutp = strtod(line.c_str(), &send); 
        saved.close();
        logcppinfo<<"MBSL8AI with addr "<<_addr<<" loaded data EInp="<<EInp<<", EOutp="<<EOutp<<ENDL;
      } else {
         logcpperror<<"File "<<_addr<<" not opened!"<<ENDL;
      }
      bLoaded = true;
    }
    virtual void save() {   //деструктор не получилось вызвать, поэтому в CleanUp прямой вызов save
      if(!bLoaded) return;
      std::fstream saved(std::to_string(_addr));
      saved<<EInp<<std::endl;
      saved<<EOutp<<std::endl;
      saved.close();
      logcppinfo<<"MBSL8AI with addr "<<_addr<<" saved data EInp="<<EInp<<", EOutp="<<EOutp<<ENDL;
    }
    suseconds_t getMillis() {
      timeval t_v;
      gettimeofday(&t_v, NULL);
      return t_v.tv_sec * 1000 + t_v.tv_usec / 1000;
    }
    double calc(int i) {
       return double(tab_reg[i]-offsets[i]) * coeff * maxs[i];
    }
    void calcAll() {
      UInp  = (2*UInp  + calc(1)) / 3;
      UBatt = (2*UBatt + calc(0)) / 3;
      IInp  = (2*IInp  + calc(5)) / 3;
      IOutp = (2*IOutp + calc(3)) / 3;
      IInv  = (3 * IInv + calc(4)) / 4;
      suseconds_t now = getMillis();
      avrg = (3 * avrg + (now-last)) >> 2;
      double time_taken = double(now - last) / double(3600000);
      last = now;
      //printf("%f\n", time_taken);
      if(PInp > 0)  EInp  += PInp  * time_taken;// считаем по прежнему значению и переводим в кВт.ч
      if(POutp > 0) EOutp += POutp * time_taken; 
      PInp  = UInp * IInp;
      POutp = UBatt * IOutp;
    }
    void resetE() { EInp = EOutp = 0; };
    virtual void poll(bool bNewDate) {
      logcppinfo<<"Polling addr "<<_addr<<ENDL;
      if(connect()) {
        if(modbus_read_input_registers(mb, 0, MBSL8AISIZE, tab_reg) == -1) {
          logcpperror<<"Read slave "<<_addr<<" failed: "<<modbus_strerror(errno)<<ENDL;
        }
        close();
        calcAll();
        if(bNewDate) {
          resetE();
          if(bLoaded) save();
          else        load();
        }
      }
    }
    std::string addValue(double value) {
      char buf[32];
      sprintf(buf, "%0.1f", value);
      return std::string(buf) + delimiter;
    }
    virtual std::string getReportString() {
      std::string ret = "";
      const double del = 1000;
      ret += addValue(UInp);
      ret += addValue(UBatt);
      ret += addValue(IInp);
      ret += addValue(IOutp);
      ret += addValue(IInv);
      ret += addValue(PInp);
      ret += addValue(POutp);
      ret += addValue(EInp/del);
      ret += addValue(EOutp/del);
      ret += std::to_string(avrg);
      return ret;
    }
};

// Проверяет смену даты для репортинга
bool checkDate() {
  time_t rawtime;
  struct tm *timeinfo;
  static int day = 100;
  time( &rawtime );                               // получить текущую дату, выраженную в секундах
  timeinfo = localtime( &rawtime );
  if(day != timeinfo->tm_mday) {
    day = timeinfo->tm_mday;
    return true;
  }
  return false;
}

// Опрашивает устройства
void pollDevices() {
  bool bNewDate = checkDate();
  for(int i=0;i<md.size();i++) {
    md[i]->poll(bNewDate);
    usleep(50000L);
  }
}

// Формирует отчет для отправки в OpenHAB
void reportDevices() {
  if(repMutex.try_lock()) {
    if(bNewReport == false) repString.erase();
    for(int i=0;i<md.size();i++) {
      repString += md[i]->getReportString();
    }
    bNewReport = true;
    repMutex.unlock();
  } else {
    logcppwarn<<"try_lock = false, setting one-time timer"<<ENDL;
    new cTimer(108, reportDevices, false);
  }
}


// Чтение основных параметров программы
void readValues(char *config_file) {
  cIniObject ini(config_file);
  initLogger(&ini);
  char *group  = (char*)"Params";
  params.destination  = ini.getString(group, (char*)"destination");
  params.type         = ini.getUInt(group, (char*)"type", MODBUS_USB);  //ms
  params.poll         = ini.getInt(group, (char*)"poll", 200);  //ms
  params.report       = ini.getInt(group, (char*)"report", 3000);  //ms
  params.rep_url      = ini.getString(group, (char*)"rep_url");
  params.bearer       = ini.getString(group, (char*)"bearer");
  params.timeout_ms   = ini.getInt(group, (char*)"timeout_ms", 700);
  group = (char*)"Addresses";
  gsize lines_ = 0;
  char **keys_;
  keys_ = ini.getKeys(group, &lines_);
  if(!lines_) logcppwarn<<"No keys in group "<<group<<ENDL;
  for(gsize i = 0;i<lines_;i++) {
    char *dev = ini.getString(group, keys_[i]);
    if(strcmp(dev, "PZEM016") == 0) md.push_back(new PZEM016(keys_[i]));
    if(strcmp(dev, "MBSL8AI") == 0) md.push_back(new MBSL8AI(keys_[i], &ini));
  }
  if(params.poll > 0) {
    new cTimer(params.poll, pollDevices, true);
  } else {
    logcpperror<<"Wrong poll parameter."<<ENDL;
  }
  if(params.report > 0) {
    new cTimer(params.report, reportDevices, true);
  } else {
    logcpperror<<"Wrong report parameter."<<ENDL;
  }
}

// Очистка
void cleanUp() {
  for(size_t i=0;i<md.size();i++) {
    md[i]->save();
  }
    if(mb) {
      modbus_close(mb);
      modbus_free(mb);
    }
    logcppwarn<<"emonitor normally finished"<<ENDL;
    if(logger) delete logger;
}

// Перехватчик SIGINT
void sigint_handler(int i) {
   puts("");
   logmessage (WARN, (char*)"Terminating program via Ctrl+C.");
   bCont.store(false);
}

int main( int argc, char **argv ) {
    char *config_file = nullptr;
    //сначала читаем только путь к конфигурационному файлу [-с path_to_file]
    //если параметры не указаны, то conf файл должен лежать рядом с исполняемым файлом
    int opt; // каждая следующая опция попадает сюда
    while ((opt = getopt(argc, argv, "c:v")) != -1) {
      switch (opt) {
        case 'c':   //путь к конф файлу. Если не задано, то ищет в том же каталоге, что и исполняемый файл
            config_file = optarg;
            break;
        case 'v':
            puts("emonitor. Collecting data from modbus");
            puts(getVersion().c_str());
            exit(EXIT_SUCCESS);
        default:
            puts("Wrong usage");
            exit(EXIT_FAILURE);
      }
    }
    readValues(config_file);
    logcppwarn<<"emonitor started."<<ENDL;
         // Подключаем обработчик SIGINT
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, 0);

    if(params.type == MODBUS_TCP) { mb = modbus_new_tcp(params.destination, 502); logcppdebug<<"TCP connection to "<<params.destination<<ENDL; }
    if(params.type == MODBUS_USB) { mb = modbus_new_rtu(params.destination, 9600, 'N', 8, 1); logcppdebug<<"USB connection to "<<params.destination<<ENDL; }
    if(!mb) {
      logcpperror<<"Modbus context creation failed: "<<modbus_strerror(errno)<<ENDL;
      bCont.store(false);
    }
    modbus_set_response_timeout(mb, 0, 100000L);

    std::thread sOH(sendToOpenHAB); //отдельный поток для отправки данных в openHAB, чтобы не замедлять темп чтения modbus-устройств
    while(bCont.load() == true) {
      timer_loop(false);
   //   usleep(timer_getSleepage()); //чтение идет так медленно, что не имеет смысла засыпать
    }
    sOH.join();
    cleanUp();
}
