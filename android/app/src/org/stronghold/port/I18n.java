package org.stronghold.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.Locale;

/** Android resources localize port UI independently of the original game's TEX. */
final class I18n {
    static final String[] LANGUAGES={"", "en", "ru", "uk", "de", "fr", "es", "it", "pl", "pt-BR", "ja", "zh-Hans"};
    private static volatile Resources resources;
    static boolean apply(Context context) {
        String language="";
        File file=new File(context.getFilesDir(),"ui-language");
        try {if(file.length()<64&&file.exists())language=new String(Files.readAllBytes(file.toPath()),StandardCharsets.US_ASCII);}
        catch(java.io.IOException ignored) { }
        if(!Arrays.asList(LANGUAGES).contains(language))language="";
        Configuration configuration=new Configuration(context.getResources().getConfiguration());
        if(!language.isEmpty())configuration.setLocale(Locale.forLanguageTag(language));
        Resources next=context.createConfigurationContext(configuration).getResources();
        boolean changed=resources==null||!resources.getConfiguration().getLocales().equals(next.getConfiguration().getLocales());
        resources=next;return changed;
    }
    static String key(String source) {return "text_"+Integer.toHexString(source.hashCode());}
    static String language() {return resources==null?"":resources.getConfiguration().getLocales().toLanguageTags();}
    static String t(String source) {
        Resources current=resources;
        if(current==null)return source;
        int id=current.getIdentifier(key(source),"string","org.stronghold.port");
        return id==0?source:current.getString(id);
    }
    static void choose(Activity activity,Runnable changed) {
        String[] labels=new String[LANGUAGES.length];labels[0]=t("Язык устройства");
        for(int i=1;i<labels.length;i++) {
            Locale locale=Locale.forLanguageTag(LANGUAGES[i]);labels[i]=locale.getDisplayName(locale);
        }
        new AlertDialog.Builder(activity).setTitle(t("Язык интерфейса"))
                .setItems(labels,(dialog,index)->{
                    File target=new File(activity.getFilesDir(),"ui-language"),temporary=new File(activity.getFilesDir(),"ui-language.tmp");
                    try {
                        Files.write(temporary.toPath(),LANGUAGES[index].getBytes(StandardCharsets.US_ASCII));
                        Files.move(temporary.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
                        apply(activity);changed.run();
                    }catch(java.io.IOException error){UcpOptions.problem(activity,error);}
                    finally {temporary.delete();}
                }).setNegativeButton(t("Отмена"),null).show();
    }
}
