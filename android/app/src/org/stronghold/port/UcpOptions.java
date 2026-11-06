package org.stronghold.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.Spinner;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONObject;

/** Both entry points edit the same atomic configuration consumed at engine startup. */
final class UcpOptions {
    static final String WEBSITE = "https://github.com/UnofficialCrusaderPatch/UnofficialCrusaderPatch";
    static JSONObject catalog(android.content.Context context) throws Exception {
        try (InputStream input = context.getAssets().open("ucp/catalog.json")) {
            java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
            byte[] bytes = new byte[8192];
            for (int n; (n=input.read(bytes))!=-1;) {
                if (buffer.size()+n>256*1024) throw new IOException(I18n.t("Слишком большой каталог UCP"));
                buffer.write(bytes,0,n);
            }
            return new JSONObject(new String(buffer.toByteArray(),java.nio.charset.StandardCharsets.UTF_8));
        }
    }
    static File config(android.content.Context context, String edition) { return new File(context.getFilesDir(),"mods-"+edition+".cfg"); }
    static void problem(Activity activity, Exception error) {
        new AlertDialog.Builder(activity).setTitle("Unofficial Crusader Patch").setMessage(error.getMessage()).setPositiveButton("OK",null).show();
    }
    static void show(Activity activity, String edition, boolean restart, Runnable applied) {
        if (GameFiles.original(edition)) return;
        try {
            JSONObject catalog=catalog(activity);
            String revision=catalog.getJSONObject("upstream").getString("framework_revision");
            Set<String> selected=GameMods.features(config(activity,edition));
            List<JSONObject> features=new ArrayList<>(),visible=new ArrayList<>();
            List<String> unavailable=new ArrayList<>(),categoryIds=new ArrayList<>(),categoryLabels=new ArrayList<>();
            JSONArray all=catalog.getJSONArray("features");
            for(int i=0;i<all.length();i++) {
                JSONObject feature=all.getJSONObject(i),support=feature.getJSONObject("editions").getJSONObject(edition);
                if(support.optBoolean("available"))features.add(feature);
                else unavailable.add(I18n.t(feature.getString("name")));
            }
            JSONArray categories=catalog.getJSONArray("categories");
            for(int i=0;i<categories.length();i++) {
                JSONObject category=categories.getJSONObject(i);int count=0;
                for(JSONObject feature:features)if(category.getString("id").equals(feature.getString("category")))count++;
                if(count>0){categoryIds.add(category.getString("id"));categoryLabels.add(I18n.t(category.getString("name"))+" ("+count+")");}
            }
            List<String> labels=new ArrayList<>();
            for(JSONObject feature:features)if(categoryIds.get(0).equals(feature.getString("category"))) {
                visible.add(feature);labels.add(I18n.t(feature.getString("name")));
            }
            ArrayAdapter<String> patches=new ArrayAdapter<>(activity,android.R.layout.simple_list_item_multiple_choice,labels);
            LinearLayout header=new LinearLayout(activity);header.setOrientation(LinearLayout.VERTICAL);
            int pad=Theme.dp(activity,16);header.setPadding(pad,pad,pad,0);
            header.addView(Theme.text(activity,"UCP · "+GameFiles.title(edition),20,Theme.TEXT,true));
            Spinner category=new Spinner(activity);
            category.setContentDescription(I18n.t("Категория патчей"));
            ArrayAdapter<String> groups=new ArrayAdapter<>(activity,android.R.layout.simple_spinner_item,categoryLabels);
            groups.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            category.setAdapter(groups);header.addView(category);
            android.widget.Button info=Theme.button(activity,I18n.t("Описание и ограничения"),false);header.addView(info);
            info.setOnClickListener(v->new AlertDialog.Builder(activity).setTitle("Unofficial Crusader Patch")
                    .setMessage(I18n.t("Удерживайте строку, чтобы прочитать описание функции.\n\nПараметры используют значения подготовленного Android-профиля. Все функции изначально выключены.\n\nПока недоступны:\n")+String.join("\n",unavailable))
                    .setPositiveButton("OK",null).show());
            if(restart)header.addView(Theme.text(activity,I18n.t("Изменения применяются с перезапуском игры."),13,Theme.MUTED,false));
            Runnable save=()->{
                try {GameMods.configureFeatures(config(activity,edition),revision,selected);applied.run();}
                catch(Exception error){problem(activity,error);}
            };
            AlertDialog dialog=new AlertDialog.Builder(activity).setCustomTitle(header)
                    .setAdapter(patches,null)
                    .setPositiveButton(restart?I18n.t("Применить и перезапустить"):I18n.t("Применить"),(d,w)->save.run())
                    .setNegativeButton(I18n.t("Отмена"),null)
                    .setNeutralButton(I18n.t("Отключить все"),(d,w)->{selected.clear();save.run();}).show();
            ListView list=dialog.getListView();list.setChoiceMode(ListView.CHOICE_MODE_MULTIPLE);
            list.setOnItemClickListener((parent,view,item,row)->{
                        try {
                            String id=visible.get(item).getString("id");boolean enabled=list.isItemChecked(item);
                            if(enabled)selected.add(id);else selected.remove(id);
                            JSONArray conflicts=catalog.getJSONObject("conflicts").getJSONArray(edition);
                            for(int n=0;n<conflicts.length();n++) {
                                JSONArray pair=conflicts.getJSONArray(n);
                                if(selected.contains(pair.getString(0))&&selected.contains(pair.getString(1))) {
                                    selected.remove(id);list.setItemChecked(item,false);
                                    throw new IOException(I18n.t("Эта функция конфликтует с уже включённой. Выберите одну из них."));
                                }
                            }
                        }catch(Exception error){problem(activity,error);}
                    });
            Runnable filter=()->{
                patches.setNotifyOnChange(false);patches.clear();visible.clear();list.clearChoices();
                for(JSONObject feature:features)if(categoryIds.get(category.getSelectedItemPosition()).equals(feature.optString("category"))) {
                    visible.add(feature);patches.add(I18n.t(feature.optString("name")));
                }
                patches.notifyDataSetChanged();
                for(int i=0;i<visible.size();i++)list.setItemChecked(i,selected.contains(visible.get(i).optString("id")));
                list.setSelection(0);
            };
            category.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){
                public void onItemSelected(AdapterView<?> parent,android.view.View view,int item,long id){filter.run();}
                public void onNothingSelected(AdapterView<?> parent){}
            });
            filter.run();
            list.setOnItemLongClickListener((parent,view,item,id)->{
                JSONObject feature=visible.get(item);
                String description=I18n.t(feature.optString("description")).trim();
                JSONArray authors=feature.optJSONArray("authors");List<String> names=new ArrayList<>();
                if(authors!=null)for(int i=0;i<authors.length();i++)names.add(authors.optJSONObject(i).optString("name"));
                if(!names.isEmpty())description+=(description.isEmpty()?"":"\n\n")+String.format(I18n.t("Авторы: %s"),String.join(", ",names));
                new AlertDialog.Builder(activity).setTitle(I18n.t(feature.optString("name"))).setMessage(description)
                        .setPositiveButton("OK",null).show();return true;
            });
        }catch(Exception error){problem(activity,error);}
    }
}
