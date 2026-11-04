// Tests the water-history storage in GreyWaterSettings.cpp against a simulated flash store.
#include "GreyWaterSettings.h"
#include <Preferences.h>
#include <cmath>
std::map<std::string,std::vector<uint8_t>> Preferences::store;
static int pass=0, fail=0;
#define CHECK(c,m) do{ if(c) ++pass; else { ++fail; printf("    FAIL: %s\n",m);} }while(0)
static void boot(){ settingsBegin(); }
static WaterStats sample(){ WaterStats s; memset(&s,0,sizeof(s)); s.version=STATS_VERSION; s.totalTapL=880.2f; s.totalReusedL=1243.7f; s.totalSavedL=1390.1f;
  s.totalCycles=52; s.monthKey=202610; s.monthReusedL=182; s.monthTapL=112.4f; s.monthSavedL=205.3f; s.monthCycles=7; s.prevMonthKey=202609;
  s.prevMonthReusedL=151; s.lastValid=1; s.lastCompleted=1; s.lastReusedL=26.4f; s.lastDurationS=3140; s.tankSinceEpoch=1761000000; return s; }
int main(){
  auto& st=Preferences::store;
  printf("H1 history survives a restart (normal firmware update)\n");
  st.clear(); boot(); stats=sample(); statsSave(); stats={}; boot();
  CHECK(stats.totalCycles==52 && std::fabs(stats.totalReusedL-1243.7f)<0.01f, "history lost after restart");
  CHECK(historySource==historyFlash, "source not 'loaded from the controller'");
  CHECK(st.count("stats_bak")==1, "no backup copy written");

  printf("H2 main record damaged: recovered from the backup copy, damaged bytes kept aside\n");
  st["stats"].resize(10); boot();
  CHECK(stats.totalCycles==52 && historySource==historyBackup, "backup copy not used");
  CHECK(st["stats_keep"].size()==10, "damaged record not kept aside");
  CHECK(st["stats"].size()==sizeof(WaterStats), "main record not repaired");

  printf("H3 unreadable format (e.g. a newer firmware) is never overwritten\n");
  st.clear(); std::vector<uint8_t> future(200,0x5A); st["stats"]=future; st["stats_bak"]=future; boot();
  CHECK(historySource==historyUnreadable && !statsHasData(stats), "unreadable record not reported");
  stats.totalCycles=1; statsSave();
  CHECK(st["stats_keep"]==future, "raw record not preserved after the next save");

  printf("H4 version 1 record from firmware 2.0-2.2 is upgraded, not deleted\n");
  st.clear(); WaterStats v=sample(); v.version=1; st["stats"]=std::vector<uint8_t>((uint8_t*)&v,(uint8_t*)&v+offsetof(WaterStats,prevMonthKey)); boot();
  CHECK(historySource==historyMigrated && stats.totalCycles==52, "v1 history not migrated");

  printf("H4b version 2 record from firmware 2.3-2.8 keeps everything and gains typical step times\n");
  st.clear(); WaterStats v2=sample(); v2.version=2; st["stats"]=std::vector<uint8_t>((uint8_t*)&v2,(uint8_t*)&v2+offsetof(WaterStats,stepAvgS)); boot();
  CHECK(historySource==historyMigrated && stats.totalCycles==52 && stats.lastValid==1 && stats.stepSamples[0]==0, "v2 history not migrated");

  printf("H5 restore from the app after a full flash erase\n");
  st.clear(); boot(); CHECK(historySource==historyEmpty && !statsHasData(stats), "erased flash not reported as empty");
  stats.tankSinceEpoch=1761500000; WaterStats b=sample();
  CHECK(statsRestore(b), "valid backup rejected");
  CHECK(historySource==historyRestored && stats.totalCycles==52, "restore not applied");
  CHECK(stats.tankSinceEpoch==1761500000, "restore overwrote the controller's own tank age");
  boot(); CHECK(stats.totalCycles==52 && std::fabs(stats.monthReusedL-182)<0.01f, "restored history not saved to flash");

  printf("H6 invalid backups are refused\n");
  WaterStats bad=sample(); bad.totalReusedL=NAN;           CHECK(!statsRestore(bad), "NaN accepted");
  bad=sample(); bad.totalTapL=-5;                          CHECK(!statsRestore(bad), "negative litres accepted");
  bad=sample(); bad.monthKey=202613;                       CHECK(!statsRestore(bad), "month 13 accepted");
  bad=sample(); bad.lastValid=7;                           CHECK(!statsRestore(bad), "bad flag accepted");
  CHECK(stats.totalCycles==52, "a refused restore changed the history");

  printf("S1 settings from 3.0 (version 6) keep every value and start in Spanish (3.1)\n");
  st.clear(); { Settings v6; settingsDefaults(v6); v6.version=6; v6.drain2DurationMs=1800000; v6.buzzerOn=1;
    strlcpy(v6.ntfyTopic,"gw-private-topic",sizeof(v6.ntfyTopic)); strlcpy(v6.webPassword,"my-new-pass",sizeof(v6.webPassword));
    v6.language=0xAB;                                     // whatever was in the padding byte
    st["cfg"]=std::vector<uint8_t>((uint8_t*)&v6,(uint8_t*)&v6+offsetof(Settings,saveMinLpm)); }   // 3.0 stored 288 bytes
  boot();
  CHECK(cfg.version==SETTINGS_VERSION && cfg.language==LANG_ES, "version 6 settings not migrated to Spanish");
  CHECK(cfg.drain2DurationMs==1800000 && cfg.buzzerOn==1 && !strcmp(cfg.ntfyTopic,"gw-private-topic") && !strcmp(cfg.webPassword,"my-new-pass"),
        "version 6 values lost in the migration");
  { Settings saved; memcpy(&saved, st["cfg"].data(), sizeof(saved)); CHECK(saved.version==SETTINGS_VERSION, "migrated settings not saved"); }
  printf("S2 a chosen language survives a restart\n");
  cfg.language=LANG_EN; settingsSave(); cfg.language=LANG_ES; boot();
  CHECK(cfg.language==LANG_EN, "English not kept after a restart");

  printf("S3 settings from 3.3 (version 7) keep every value and start with the default screen font\n");
  st.clear(); { Settings v7; settingsDefaults(v7); v7.version=7; v7.language=LANG_EN; v7.drain2DurationMs=1500000;
    strlcpy(v7.ntfyServer,"https://ntfy.envs.net",sizeof(v7.ntfyServer));
    v7.screenFont=0xCD;                                   // whatever was in the padding byte
    st["cfg"]=std::vector<uint8_t>((uint8_t*)&v7,(uint8_t*)&v7+offsetof(Settings,saveMinLpm)); }   // 3.3 stored 288 bytes
  boot();
  CHECK(cfg.version==SETTINGS_VERSION && cfg.screenFont==0, "version 7 settings not migrated to the default screen font");
  CHECK(cfg.language==LANG_EN && cfg.drain2DurationMs==1500000 && !strcmp(cfg.ntfyServer,"https://ntfy.envs.net"), "version 7 values lost in the migration");
  printf("S4 a chosen screen font survives a restart; a bad value falls back to the default\n");
  cfg.screenFont=3; settingsSave(); cfg.screenFont=0; boot();
  CHECK(cfg.screenFont==3, "screen font not kept after a restart");
  cfg.screenFont=4; settingsSanitize(cfg);
  CHECK(cfg.screenFont==0, "screen font 4 accepted (3.6 has fonts 0-3)");
  printf("S5 minimum save flow: 0.5 L/min after the upgrade, kept after a restart, limited to 0.5-10 (3.4)\n");
  CHECK(cfg.saveMinLpm==0.5f, "upgraded settings do not save above 0.5 L/min");
  cfg.saveMinLpm=1.5f; settingsSave(); cfg.saveMinLpm=0.5f; boot();
  CHECK(cfg.saveMinLpm==1.5f, "minimum save flow not kept after a restart");
  cfg.saveMinLpm=0.2f; settingsSanitize(cfg); CHECK(cfg.saveMinLpm==0.5f, "0.2 L/min accepted (minimum 0.5)");
  cfg.saveMinLpm=0.0f/0.0f; settingsSanitize(cfg); CHECK(cfg.saveMinLpm==0.5f, "NaN accepted");

  printf("S6 settings from 3.4 (version 8) keep every value; the viewer user starts off (3.5), the screen font is the new default (3.6)\n");
  st.clear(); { Settings v8; settingsDefaults(v8); v8.version=8; v8.screenFont=3; v8.saveMinLpm=2.0f;
    strlcpy(v8.webPassword,"owner-pass-1",sizeof(v8.webPassword));
    st["cfg"]=std::vector<uint8_t>((uint8_t*)&v8,(uint8_t*)&v8+offsetof(Settings,viewerPassword)); }   // 3.4 stored up to saveMinLpm
  boot();
  CHECK(cfg.version==SETTINGS_VERSION && cfg.viewerPassword[0]=='\0', "version 8 settings not migrated with the viewer off");
  CHECK(cfg.screenFont==0, "version 8 screen font kept: its number named a font removed in 3.6");
  CHECK(cfg.saveMinLpm==2.0f && !strcmp(cfg.webPassword,"owner-pass-1"), "version 8 values lost in the migration");
  printf("S7 a viewer password survives a restart\n");
  strlcpy(cfg.viewerPassword,"family-view-1",sizeof(cfg.viewerPassword)); settingsSave(); cfg.viewerPassword[0]='\0'; boot();
  CHECK(!strcmp(cfg.viewerPassword,"family-view-1"), "viewer password not kept after a restart");
  printf("S8 older settings (version 6) start with the viewer off\n");
  st.clear(); { Settings v6; memset(&v6,0x5A,sizeof(v6)); settingsDefaults(v6); v6.version=6;
    memset(v6.viewerPassword,'x',sizeof(v6.viewerPassword));   // bytes beyond what 3.0 stored must not become a password
    st["cfg"]=std::vector<uint8_t>((uint8_t*)&v6,(uint8_t*)&v6+offsetof(Settings,saveMinLpm)); }
  boot();
  CHECK(cfg.viewerPassword[0]=='\0', "version 6 settings migrated with a viewer password");

  printf("S9 settings from 3.5 (version 9) keep every value; the screen font is the new default (3.6)\n");
  st.clear(); { Settings v9; settingsDefaults(v9); v9.version=9; v9.screenFont=2; v9.saveMinLpm=1.5f; v9.language=LANG_EN;
    strlcpy(v9.viewerPassword,"family-view-2",sizeof(v9.viewerPassword));
    st["cfg"]=std::vector<uint8_t>((uint8_t*)&v9,(uint8_t*)&v9+sizeof(Settings)); }   // same size as version 10
  boot();
  CHECK(cfg.version==SETTINGS_VERSION && cfg.screenFont==0, "version 9 settings not migrated to the default screen font");
  CHECK(cfg.saveMinLpm==1.5f && cfg.language==LANG_EN && !strcmp(cfg.viewerPassword,"family-view-2"), "version 9 values lost in the migration");
  { Settings saved; memcpy(&saved, st["cfg"].data(), sizeof(saved)); CHECK(saved.version==SETTINGS_VERSION, "version 9 migration not saved"); }

  printf("\n%d checks passed, %d failed\n", pass, fail);
  return fail?1:0;
}
