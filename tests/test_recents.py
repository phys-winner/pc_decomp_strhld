"""Run the real task cleanup, edition handoff and launcher return on the JVM."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "android/app/src/org/stronghold/port"


def method(source, declaration):
    match = re.search(r"^    " + re.escape(declaration) + r"[^\n]*\{.*?^    }", source, re.M | re.S)
    assert match, declaration
    return match.group()


def main():
    launcher = (APP / "LauncherActivity.java").read_text(encoding="utf-8")
    game = (APP / "GameActivity.java").read_text(encoding="utf-8")
    settings = (APP / "SettingsActivity.java").read_text(encoding="utf-8")
    assert "keepCurrentTask(this);" in method(launcher, "@Override public void onCreate(")
    for source in (game, settings):
        assert "LauncherActivity.keepCurrentTask(this);" in method(source, "@Override public void onCreate(")
    assert "FLAG_ACTIVITY_NEW_TASK" not in settings
    program = r'''
import java.io.*;import java.util.*;
class Context {
    static final String ACTIVITY_SERVICE="activity";
    Object getSystemService(String name){throw new AssertionError("not an activity");}
}
class Activity extends Context {
    static File folder;ActivityManager manager=new ActivityManager();Intent launched;boolean finished,finishing,destroyed;
    Object getSystemService(String name){return manager;}int getTaskId(){return 7;}
    boolean isFinishing(){return finishing;}boolean isDestroyed(){return destroyed;}
    File getFilesDir(){return folder;}
    void startActivity(Intent intent){launched=intent;}
    void startActivity(Intent intent,Object options){startActivity(intent);}
    void finish(){assert launched!=null;finished=true;}
    void finishAndRemoveTask(){throw new AssertionError("must preserve the shared task");}
    void overridePendingTransition(int enter,int exit){assert enter==0&&exit==0;}
}
class ActivityManager {
    List<AppTask> tasks=new ArrayList<>();List<AppTask> getAppTasks(){return tasks;}
    static class RecentTaskInfo {int persistentId;RecentTaskInfo(int id){persistentId=id;}}
    static class AppTask {
        int id;boolean gone,removed;AppTask(int id){this.id=id;}
        RecentTaskInfo getTaskInfo(){if(gone)throw new IllegalArgumentException();return new RecentTaskInfo(id);}
        void finishAndRemoveTask(){assert id!=7;removed=true;}
    }
}
class Intent {
    static final int FLAG_ACTIVITY_REORDER_TO_FRONT=0x20000,FLAG_ACTIVITY_SINGLE_TOP=0x20000000,FLAG_ACTIVITY_NO_ANIMATION=0x10000;
    Class<?> target;int flags;Map<String,Object> extras=new HashMap<>();
    Intent(Context c,Class<?> target){this.target=target;}
    Intent putExtra(String key,Object value){extras.put(key,value);return this;}
    Intent addFlags(int value){flags|=value;return this;}
}
class android {
    static class util {static class Log {static void w(String tag,String text,Throwable error){}}}
    static class app {static class ActivityOptions {
        static ActivityOptions makeCustomAnimation(Context c,int enter,int exit){return new ActivityOptions();}
        Object toBundle(){return this;}
    }}
    static class os {static class Process {static int killed;static int myPid(){return 123;}static void killProcess(int id){assert id==123;killed++;}}}
}
class GameFiles {static File pending(File folder,String id){return new File(folder,id+".pending");}}
class CrashMonitor {
    static final String OPTION="monitorCrashes";static int ended;
    static void end(Activity activity,String edition){assert "classic".equals(edition);ended++;}
}
class I18n {static String t(String text){return text;}}
class LauncherActivity extends Activity {
    boolean showingLauncher=true,selectingFiles=true,existing;int scanVersion;
    static class Preferences extends HashMap<String,Boolean> {
        boolean getBoolean(String key,boolean fallback){return getOrDefault(key,fallback);}
    }
    final Preferences preferences=new Preferences();
    boolean running(String item){return existing;}
    Class<?> gameClass(String item){return GameActivity.class;}
    void showLaunchError(String item,String text){throw new AssertionError(text);}
/* LAUNCHER */
}
class GameActivity extends Activity {
    String edition="classic";void releaseControls(){}
/* GAME */
}
public class RecentsCheck {
    static void returned(GameActivity game){
        assert game.launched.target==LauncherActivity.class;
        assert game.launched.flags==(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT|Intent.FLAG_ACTIVITY_SINGLE_TOP);
    }
    public static void main(String[] args){
        Activity.folder=new File(args[0]);Activity current=new Activity();
        ActivityManager.AppTask old=new ActivityManager.AppTask(3),same=new ActivityManager.AppTask(7),
            gone=new ActivityManager.AppTask(9),other=new ActivityManager.AppTask(11);
        gone.gone=true;current.manager.tasks.addAll(Arrays.asList(old,same,gone,other));
        LauncherActivity.keepCurrentTask(current);assert old.removed&&other.removed&&!same.removed;
        old.removed=false;current.finishing=true;LauncherActivity.keepCurrentTask(current);assert !old.removed;
        current.finishing=false;current.destroyed=true;LauncherActivity.keepCurrentTask(current);assert !old.removed;
        LauncherActivity.keepCurrentTask(new Context());
        for(boolean running:new boolean[]{false,true}){
            LauncherActivity launcher=new LauncherActivity();launcher.existing=running;
            if (running) launcher.preferences.put(CrashMonitor.OPTION, false);
            launcher.launch("classic","game","images",true);
            assert launcher.finished&&!launcher.showingLauncher&&!launcher.selectingFiles;
            assert launcher.launched.target==GameActivity.class;
            assert launcher.launched.flags==(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT|Intent.FLAG_ACTIVITY_SINGLE_TOP|Intent.FLAG_ACTIVITY_NO_ANIMATION);
            assert "game".equals(launcher.launched.extras.get("game"));
            assert Boolean.valueOf(!running).equals(launcher.launched.extras.get(CrashMonitor.OPTION));
        }
        GameActivity chosen=new GameActivity();chosen.chooseEdition();returned(chosen);assert !chosen.finished;
        GameActivity switched=new GameActivity();switched.launchEdition("extreme");returned(switched);assert switched.finished;
        assert "extreme".equals(switched.launched.extras.get("edition"))&&Boolean.TRUE.equals(switched.launched.extras.get("launchGame"));
        GameActivity failed=new GameActivity();failed.launchError("failed");returned(failed);assert failed.finished;
        assert "failed".equals(failed.launched.extras.get("error"));
        GameActivity exited=new GameActivity();exited.returnToLauncher();returned(exited);assert exited.finished;
        assert android.os.Process.killed==3&&CrashMonitor.ended==2;
        System.out.println("PASS Stronghold recents: old task cleanup, cold/resumed play, launcher overlay, edition switch, error and clean exit preserve one task");
    }
}
'''
    methods = "\n".join(method(launcher, name) for name in ("static void keepCurrentTask(", "private void launch("))
    program = program.replace("/* LAUNCHER */", methods.replace("private void launch(", "void launch("))
    methods = "\n".join(method(game, name) for name in (
        "private void chooseEdition(", "private Intent launcherIntent(", "private void launchEdition(",
        "private void launchError(", "private void returnToLauncher("))
    program = program.replace("/* GAME */", methods.replace("private void ", "void "))
    with tempfile.TemporaryDirectory(prefix="stronghold-recents-") as temporary:
        folder = Path(temporary)
        source = folder / "RecentsCheck.java"
        source.write_text(program, encoding="utf-8")
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        subprocess.run([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(folder), str(source)], check=True)
        subprocess.run([str(java / "java.exe"), "-ea", "-cp", str(folder), "RecentsCheck", str(folder)], check=True)


if __name__ == "__main__":
    main()
