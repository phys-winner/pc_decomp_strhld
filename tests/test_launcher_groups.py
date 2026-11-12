"""Exercise the launcher's folding, inline notices and progress reattachment on the JVM."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "android/app/src/org/stronghold/port/LauncherActivity.java").read_text(encoding="utf-8")
methods = []
for name in ("private void foldFiles(", "private void setStatus(TextView target,", "private static void attach("):
    match = re.search(r"^    " + re.escape(name) + r"[^\n]*\{.*?^    }", source, re.M | re.S)
    assert match, name
    methods.append(match.group().replace("android.view.ViewGroup", "ViewGroup"))
program = r'''
import java.util.*;
class View {
    static final int VISIBLE=0,GONE=8;int visibility;ViewGroup parent;
    void setVisibility(int v){visibility=v;}int getVisibility(){return visibility;}Object getParent(){return parent;}
}
class ViewGroup extends View {
    List<View> children=new ArrayList<>();
    void addView(View v){assert v.parent==null;children.add(v);v.parent=this;}
    void removeView(View v){assert children.remove(v);v.parent=null;}
    void removeAllViews(){for(View v:children)v.parent=null;children.clear();}
}
class LinearLayout extends ViewGroup {}
class TextView extends View {
    String text="";int colour;void setText(String t){text=t==null?"":t;}void setTextColor(int c){colour=c;}
    CharSequence getText(){return text;}int getCurrentTextColor(){return colour;}
}
class Button extends TextView {
    java.util.function.Consumer<View> click;void setOnClickListener(java.util.function.Consumer<View> c){click=c;}
}
class Theme {static final int BAD=1,MUTED=2;static Button button(Object c,String text,boolean primary){return new Button();}}
class I18n {static String t(String s){return s;}}
class Preferences {
    int writes;Preferences edit(){return this;}Preferences putString(String key,String value){++writes;return this;}void apply(){}
}
public class LauncherGroups {
    boolean filesExpanded,checking,pickingGog;
    final TextView filesHeading=new TextView(),status=new TextView();
    final TextView diagnosticNote=new TextView();
    final LinearLayout fileDetails=new LinearLayout(),fileProgress=new LinearLayout();
    final Map<TextView,LinearLayout> recoveryActions=new HashMap<>();final Preferences preferences=new Preferences();
    int folders,archives,copies;boolean requestStorage(){return false;}
    void pickFolder(){++folders;}void pickArchive(){++archives;}void copyDiagnostics(){++copies;}
''' + "\n".join(methods) + r'''
    public static void main(String[] args){
        LauncherGroups c=new LauncherGroups();c.fileProgress.setVisibility(View.GONE);c.foldFiles();
        assert c.fileDetails.getVisibility()==View.GONE;
        c.filesExpanded=true;c.foldFiles();assert c.fileDetails.getVisibility()==View.VISIBLE;
        c.fileProgress.setVisibility(View.VISIBLE);c.foldFiles();assert c.fileDetails.getVisibility()==View.GONE;
        LinearLayout actions=new LinearLayout();c.recoveryActions.put(c.status,actions);
        c.setStatus(c.status,"failed",true);assert c.status.colour==Theme.BAD&&c.status.visibility==View.VISIBLE;
        assert actions.children.size()==2&&c.preferences.writes==1;
        ((Button)actions.children.get(0)).click.accept(null);((Button)actions.children.get(1)).click.accept(null);
        assert c.folders==1&&c.archives==1;
        View first=actions.children.get(0);c.setStatus(c.status,"failed",true);
        assert c.preferences.writes==1&&actions.children.get(0)==first : "unchanged errors must preserve controls";
        c.filesExpanded=false;c.foldFiles();assert c.fileProgress.visibility==View.VISIBLE&&c.status.visibility==View.VISIBLE;
        LinearLayout oldCard=new LinearLayout(),newCard=new LinearLayout();
        attach(oldCard,c.fileProgress);attach(newCard,c.fileProgress);
        assert oldCard.children.isEmpty()&&c.fileProgress.parent==newCard;
        c.fileProgress.setVisibility(View.GONE);c.filesExpanded=true;c.foldFiles();
        assert c.fileDetails.visibility==View.VISIBLE;
        c.setStatus(c.status,null,false);assert c.status.visibility==View.GONE;
        assert actions.children.isEmpty();
        c.recoveryActions.put(c.diagnosticNote,actions);c.setStatus(c.diagnosticNote,"clipboard unavailable",true);
        ((Button)actions.children.get(0)).click.accept(null);assert c.copies==1;
        System.out.println("PASS launcher groups: default folding, progress replaces controls, inline errors, progress survives reattachment");
    }
}
'''
with tempfile.TemporaryDirectory(prefix="stronghold-launcher-groups-") as temporary:
    folder = Path(temporary)
    path = folder / "LauncherGroups.java"
    path.write_text(program, encoding="utf-8")
    java = Path(os.environ["JAVA_HOME"]) / "bin"
    subprocess.run(list(map(str, [java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", folder, path])), check=True)
    subprocess.run(list(map(str, [java / "java.exe", "-ea", "-cp", folder, "LauncherGroups"])), check=True)
