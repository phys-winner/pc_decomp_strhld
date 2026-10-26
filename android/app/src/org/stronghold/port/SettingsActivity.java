package org.stronghold.port;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.preference.CheckBoxPreference;
import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceActivity;
import android.preference.PreferenceCategory;
import android.preference.PreferenceGroup;
import android.preference.PreferenceScreen;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Toast;

/** Platform preferences, in the edition's process so live games see changes immediately. */
public abstract class SettingsActivity extends PreferenceActivity {
    protected abstract String edition();
    private SharedPreferences preferences;
    private PreferenceScreen home, graphics, audio, gameplay;
    private String section = "";
    private int[] game;
    static Class<? extends SettingsActivity> settingsClass(String edition) {
        return "classic".equals(edition) ? ClassicSettingsActivity.class : "stronghold".equals(edition) ? StrongholdSettingsActivity.class : ExtremeSettingsActivity.class;
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        /* Like the launcher: the backdrop also fills the camera cutout and bars,
         * while the action bar and list keep clear of them. */
        Theme.edgeToEdge(this);
        View root = ((ViewGroup) getWindow().getDecorView()).getChildAt(0);
        root.setOnApplyWindowInsetsListener((view, insets) -> Theme.padInsets(view, insets, 0, 0, 0, 0));
        setTitle("Настройки · " + GameFiles.title(edition()));
        getActionBar().setDisplayHomeAsUpEnabled(true);
        preferences = getSharedPreferences("controls-" + edition(), MODE_PRIVATE);
        migrate(preferences);
        cleanRemovedMenu(preferences);
        GameActivity.forgetOtherBuildsPerformance(this, preferences);
        game = GameActivity.gameSettings();
        if (game != null && state == null) {
            /* A running game is authoritative: its own controls may have changed these. */
            SharedPreferences.Editor remembered = preferences.edit();
            for (int i=0;i<game.length;i++) {
                remembered.putInt("last-game-"+i,game[i]);
                if (preferences.contains("game-"+i)) remembered.putInt("game-"+i,game[i]);
            }
            remembered.commit();
        }
        buildScreens();
        showSection(state == null ? getIntent().getStringExtra("section") : state.getString("section"));
    }
    private void buildScreens() {
        home = getPreferenceManager().createPreferenceScreen(this);
        graphics = newSection("graphics", "Графика", "Разрешение, размер интерфейса и качество изображения");
        audio = newSection("audio", "Звук", "Музыка, эффекты и речь");
        gameplay = newSection("game", "Игра", "Скорость игры, подсказки и управление");
        if (GameFiles.original(edition())) strongholdSettings(graphics); else crusaderGraphics(graphics);
        graphicsDescriptions();
        minimumGraphics(graphics);
        gameSettings();
        controlSettings();
        resetSettings();
        Preference launcher = new Preference(this); launcher.setTitle("Лаунчер и папки игры"); home.addPreference(launcher);
        launcher.setOnPreferenceClickListener(item -> {
            startActivity(new Intent(this,LauncherActivity.class).putExtra("showLauncher",true).putExtra("edition",edition())
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP)); return true;
        });
    }
    private void crusaderGraphics(PreferenceGroup graphics) {
        list(graphics,"resolution","Разрешение",new String[]{"Авто","Минимальное","Устройство","720p","1080p","1440p"},
                new String[]{"auto","minimum","device","1280x720","1920x1080","2560x1440"},preferences.getString("resolution","auto"),
                value -> preferences.edit().putString("resolution",value).apply());
        Preference note = new Preference(this); note.setTitle("Разрешение и пропорции применяются при новом запуске игры");
        note.setSummary("Перед перезапуском сохраните матч. «Минимальное» — 600 строк, наименьший кадр движка, с пропорциями экрана. "+
                "Разрешение не меняется автоматически во время игры."); note.setSelectable(false); graphics.addPreference(note);
        toggle(graphics,"wide","Использовать пропорции устройства",true);
        resolutionLabels();
        integers(graphics,"uiScale","Размер интерфейса",new int[]{75,90,100,110,120,150,175,200},120,"%",-1);
        if ("extreme".equals(edition()))
            integers(graphics,"powersScale","Размер панели сил",new int[]{75,100,120,150,175,200,250,300},150,"%",-1);
        String[] filterLabels={"FSR 1.0","Бикубический","Чёткие пиксели","Билинейный"},filterValues={"fsr","bicubic","pixels","bilinear"};
        list(graphics,"worldFilter","Фильтр мира",filterLabels,filterValues,preferences.getString("worldFilter","fsr"),
                value -> preferences.edit().putString("worldFilter",value).apply());
        list(graphics,"uiFilter","Фильтр интерфейса",filterLabels,filterValues,preferences.getString("uiFilter","fsr"),
                value -> preferences.edit().putString("uiFilter",value).apply());
        integers(graphics,"sharpness","Резкость",new int[]{0,25,50,75,100},25,"%",-1);
        toggle(graphics,"deband","Сглаживание градиентов",false);
        toggle(graphics,"crispText","Чёткий текст",true);
    }
    private void graphicsDescriptions() {
        String filters = "FSR 1.0 сохраняет чёткие края, бикубический мягче, «Чёткие пиксели» сохраняют пиксельный вид без зубцов, билинейный самый мягкий. Применяется сразу.";
        describe("worldFilter", "Карта увеличивается до размера экрана отдельно от интерфейса. " + filters);
        describe("uiFilter", "Интерфейс увеличивается до размера экрана отдельно от карты. " + filters);
        describe("sharpness", "Сила повышения резкости изображения. Применяется сразу.");
        describe("deband", "Убирает ступеньки 16-битного цвета на небе и воде. Применяется сразу.");
        describe("crispText", "Перерисовывает надписи в разрешении экрана. Применяется сразу.");
        describe("uiScale", "Размер панели, окон, ресурсов и подсказок в матче. Ограничивается размером кадра, чтобы интерфейс помещался целиком. Применяется сразу.");
        if ("extreme".equals(edition())) describe("powersScale", "Размер боковой панели сил независимо от основного интерфейса. Применяется сразу.");
    }
    private void describe(String key, String description) {
        Preference item = findSetting(key);
        item.setSummary(item instanceof ListPreference ? "%s\n" + description.replace("%", "%%") : description);
    }
    private void gameSettings() {
        int minimum = GameFiles.original(edition()) ? 10 : 20;
        int[] speeds = new int[(90-minimum)/5+1];
        for(int i=0;i<speeds.length;i++) speeds[i]=minimum+i*5;
        integers(gameplay,"game-0","Скорость игры",speeds,nativeValue(0,40),"",0);
        gameToggle(gameplay,1,"Всплывающие подсказки",true);
        toggle(gameplay,"smoothCamera","Плавное перемещение камеры",true);
        namedGame(gameplay,2,"Скорость прокрутки",new String[]{"Медленно","Нормально","Быстро"},new int[]{0,1,2},0);
        namedGame(gameplay,3,"Курсор",new String[]{"Стрелка","Меч"},new int[]{1,2},2);
        gameToggle(audio,4,"Звук включён",true);
        int[] volumes = new int[21]; for (int i=0;i<volumes.length;i++) volumes[i]=i*5;
        integers(audio,"game-5","Громкость музыки",volumes,nativeValue(5,90),"%",5);
        integers(audio,"game-6","Громкость эффектов",volumes,nativeValue(6,80),"%",6);
        integers(audio,"game-7","Громкость речи",volumes,nativeValue(7,85),"%",7);
        if (!GameFiles.original(edition())) gameToggle(audio,8,"Джинн",true);
    }
    private void controlSettings() {
        PreferenceCategory controls = category(gameplay,"Управление");
        toggle(controls,"touchpad","Тачпад с курсором",false);
        floatChoices(controls,"sensitivity","Чувствительность тачпада",new float[]{.5f,.75f,1,1.25f,1.5f,2,2.5f},1,"×");
        integers(controls,"controlsScale","Размер кнопок",new int[]{75,90,100,110,120,130,140,150,175,200},100,"%",-1);
        floatChoices(controls,"opacity","Видимость кнопок",new float[]{.25f,.5f,.75f,1},.75f,"");
        Preference buttons = new Preference(this); buttons.setTitle("Кнопки сенсорного управления");
        buttons.setSummary("Отдельные кнопки на экране, в боковых списках и книге писаря. Любую кнопку можно скрыть; ЛКМ по умолчанию скрыта.");
        controls.addPreference(buttons);
        buttons.setOnPreferenceClickListener(item -> { controlChoices(this, preferences, () -> {}); return true; });
        Preference zones = new Preference(this); zones.setTitle("Боковые зоны");
        zones.setSummary("Список команд или отдельные действия по касанию и удержанию для каждой зоны");
        controls.addPreference(zones);
        zones.setOnPreferenceClickListener(item -> {
            new AlertDialog.Builder(this).setTitle("Боковые зоны").setItems(new String[]{"Левая зона", "Правая зона"},
                    (dialog, side) -> zoneChoices(this, preferences, side, () -> {})).setNegativeButton("Готово", null).show();
            return true;
        });
        toggle(controls,"buildingSwipes","Переключать разделы построек свайпами",true);
        loupeSettings(controls);
        toggle(controls,"vibration","Вибрация",true);
        toggle(controls,"touchDiagnostics","Диагностика касания",false);
        Preference arrange = new Preference(this); arrange.setTitle("Изменить расположение кнопок");
        arrange.setSummary("Предпросмотр поверх паузы: перемещение, размер щипком или ползунком, Сохранить и Отмена");
        controls.addPreference(arrange);
        arrange.setOnPreferenceClickListener(item -> {
            if (!GameActivity.gameRunning()) {
                Toast.makeText(this, "Сначала запустите игру: расположение меняется поверх игрового экрана.", Toast.LENGTH_LONG).show();
                return true;
            }
            preferences.edit().putBoolean("layoutEditRequest", true).commit(); finish(); return true;
        });
        Preference reset = new Preference(this); reset.setTitle("Сбросить расположение кнопок");
        controls.addPreference(reset);
        reset.setOnPreferenceClickListener(item -> {
            SharedPreferences.Editor edit = preferences.edit();
            for (String key : preferences.getAll().keySet()) if (key.startsWith("pos-")) edit.remove(key);
            edit.apply(); Toast.makeText(this, "Исходное расположение восстановлено", Toast.LENGTH_SHORT).show(); return true;
        });
    }
    static int controlPlacement(SharedPreferences preferences, String id) {
        return ControlLayout.placement(preferences.getAll(), id);
    }
    static void zoneChoices(android.content.Context context, SharedPreferences preferences, int side, Runnable changed) {
        if (side < 0 || side > 1) return;
        boolean direct = ControlLayout.zoneDirect(preferences.getAll(), side);
        String prefix = side == 0 ? "zone-left-" : "zone-right-";
        String[] modes = {"Список команд", "Действия по тапу и удержанию"};
        String tap = ControlLayout.zoneAction(preferences.getAll(), side, false), hold = ControlLayout.zoneAction(preferences.getAll(), side, true);
        String[] choices = direct ? new String[]{"Режим: " + modes[1], "Тап: " + (tap.isEmpty() ? "Ничего" : ControlLayout.label(tap)),
                "Удержание: " + (hold.isEmpty() ? "Ничего" : ControlLayout.label(hold))} : new String[]{"Режим: " + modes[0], "Команды списка"};
        new AlertDialog.Builder(context).setTitle(side == 0 ? "Левая боковая зона" : "Правая боковая зона")
                .setItems(choices, (dialog, which) -> {
                    if (which == 0) {
                        new AlertDialog.Builder(context).setTitle("Режим боковой зоны")
                                .setSingleChoiceItems(modes, direct ? 1 : 0, (choice, selected) -> {
                                    preferences.edit().putString(prefix + "mode", selected == 1 ? "actions" : "list").apply();
                                    changed.run(); choice.dismiss(); zoneChoices(context, preferences, side, changed);
                                }).setNegativeButton("Назад", (choice, selected) -> zoneChoices(context, preferences, side, changed)).show();
                    } else if (!direct) controlChoices(context, preferences, changed);
                    else zoneActionChoices(context, preferences, side, which == 2, changed);
                }).setNegativeButton("Готово", null).show();
    }
    private static void zoneActionChoices(android.content.Context context, SharedPreferences preferences, int side, boolean hold, Runnable changed) {
        java.util.ArrayList<String> ids = new java.util.ArrayList<>(), names = new java.util.ArrayList<>();
        ids.add(""); names.add("Ничего");
        for (String id : ControlLayout.IDS) if (ControlLayout.inZone(id)) { ids.add(id); names.add(ControlLayout.label(id)); }
        new AlertDialog.Builder(context).setTitle(hold ? "Действие по удержанию" : "Действие по тапу")
                .setItems(names.toArray(new String[0]), (dialog, which) -> {
                    preferences.edit().putString((side == 0 ? "zone-left-" : "zone-right-") + (hold ? "hold" : "tap"), ids.get(which)).apply();
                    changed.run(); zoneChoices(context, preferences, side, changed);
                }).setNegativeButton("Назад", (dialog, which) -> zoneChoices(context, preferences, side, changed)).show();
    }
    static void cleanRemovedMenu(SharedPreferences preferences) {
        java.util.Map<String, ?> before = preferences.getAll();
        java.util.Map<String, Object> values = new java.util.HashMap<>(before);
        if (!ControlLayout.cleanRemovedMenu(values)) return;
        SharedPreferences.Editor edit = preferences.edit();
        for (String key : before.keySet()) {
            if (!values.containsKey(key)) edit.remove(key);
            else if (!java.util.Objects.equals(before.get(key), values.get(key))) edit.putInt(key, (Integer)values.get(key));
        }
        edit.apply();
    }
    static void controlChoices(android.content.Context context, SharedPreferences preferences, Runnable changed) {
        String[] places = {"На экране", "Скрыта", "В левой зоне", "В правой зоне", "В книге писаря"};
        int[] placements = {ControlLayout.SCREEN, ControlLayout.HIDDEN, ControlLayout.LEFT_ZONE, ControlLayout.RIGHT_ZONE, ControlLayout.BOOK_ZONE};
        String[] names = new String[ControlLayout.IDS.length];
        for (int i = 0; i < names.length; i++) {
            int current = controlPlacement(preferences, ControlLayout.IDS[i]), selected = 1;
            for (int j = 0; j < placements.length; j++) if (placements[j] == current) selected = j;
            names[i] = ControlLayout.LABELS[i] + " · " + places[selected];
        }
        new AlertDialog.Builder(context).setTitle("Выберите кнопку")
                .setItems(names, (dialog, index) -> {
                    String id = ControlLayout.IDS[index];
                    String[] choices = ControlLayout.inZone(id) ? places : new String[]{places[0], places[1]};
                    int[] values = ControlLayout.inZone(id) ? placements : new int[]{ControlLayout.SCREEN, ControlLayout.HIDDEN};
                    int current = controlPlacement(preferences, id), selected = 1;
                    for (int i = 0; i < values.length; i++) if (values[i] == current) selected = i;
                    new AlertDialog.Builder(context).setTitle(ControlLayout.LABELS[index])
                            .setSingleChoiceItems(choices, selected,
                                    (choice, which) -> {
                                        preferences.edit().putInt("control-" + id, values[which]).apply();
                                        changed.run(); choice.dismiss(); controlChoices(context, preferences, changed);
                                    }).setNegativeButton("Назад", (choice, which) -> controlChoices(context, preferences, changed)).show();
                }).setNeutralButton("Исходные кнопки", (dialog, which) -> {
                    SharedPreferences.Editor edit = preferences.edit();
                    for (String id : ControlLayout.IDS) edit.remove("control-" + id);
                    for (String prefix : new String[]{"zone-left-", "zone-right-"})
                        edit.remove(prefix + "mode").remove(prefix + "tap").remove(prefix + "hold");
                    edit.remove("extra").remove("groupWheel").remove("markerWheel").apply();
                    changed.run(); controlChoices(context, preferences, changed);
                }).setNegativeButton("Готово", null).show();
    }
    /* Stronghold HD uses its own mode table and native option handlers. */
    private void strongholdSettings(PreferenceGroup graphics) {
        /* A former "1280x720" choice keeps its height. */
        String saved=preferences.getString("resolution","720");
        if(saved.contains("x")){saved=Integer.toString(GameActivity.strongholdMode(saved,1,1,false)[1]);preferences.edit().putString("resolution",saved).apply();}
        String[] heights=new String[GameActivity.STRONGHOLD_HEIGHTS.length];
        for(int i=0;i<heights.length;i++)heights[i]=Integer.toString(GameActivity.STRONGHOLD_HEIGHTS[i]);
        list(graphics,"resolution","Разрешение",heights,heights,saved,value -> preferences.edit().putString("resolution",value).apply());
        toggle(graphics,"wide","Во весь экран",true);
        strongholdResolutionLabels();
        Preference note = new Preference(this); note.setTitle("Применяется при новом запуске игры");
        note.setSummary("Высота кадра — из режимов самой игры. «Во весь экран»: ширина кадра в пропорциях экрана, без полей по бокам. "+
                "Выключено — режимы игры 16:9 (600 строк — 4:3) с полями."); note.setSelectable(false); graphics.addPreference(note);
        integers(graphics,"uiScale","Размер интерфейса",new int[]{100,110,120,130,150,175,200},100,"%",-1);
        String[] filterLabels={"FSR 1.0","Бикубический","Чёткие пиксели","Билинейный"},filterValues={"fsr","bicubic","pixels","bilinear"};
        list(graphics,"worldFilter","Фильтр карты",filterLabels,filterValues,preferences.getString("worldFilter","fsr"),
                value -> preferences.edit().putString("worldFilter",value).apply());
        list(graphics,"uiFilter","Фильтр интерфейса и меню",filterLabels,filterValues,preferences.getString("uiFilter","fsr"),
                value -> preferences.edit().putString("uiFilter",value).apply());
        integers(graphics,"sharpness","Резкость",new int[]{0,25,50,75,100},25,"%",-1);
        toggle(graphics,"deband","Сглаживание градиентов",false);
        toggle(graphics,"crispText","Чёткий текст",true);
    }
    private void minimumGraphics(PreferenceGroup graphics) {
        Preference item=new Preference(this);item.setTitle("Установить минимальную графику");
        item.setSummary("Минимальное разрешение, билинейные фильтры, без резкости, сглаживания градиентов и чёткого текста. Разрешение — после перезапуска игры.");
        graphics.addPreference(item);
        item.setOnPreferenceClickListener(clicked -> {
            new AlertDialog.Builder(this).setTitle("Установить минимальную графику?")
                    .setMessage("Будут выбраны минимальное разрешение и билинейные фильтры, отключены резкость, сглаживание градиентов и чёткий текст. Разрешение изменится после перезапуска игры; перед ним сохраните матч.")
                    .setNegativeButton("Отмена", null).setPositiveButton("Установить", (dialog, which) -> applyMinimumGraphics()).show();
            return true;
        });
    }
    private void applyMinimumGraphics() {
        String resolution=GameFiles.original(edition())?"600":"minimum";
        preferences.edit().putString("resolution",resolution)
                .putString("worldFilter","bilinear").putString("uiFilter","bilinear")
                .putInt("sharpness",0).putBoolean("deband",false).putBoolean("crispText",false).commit();
        ((ListPreference)findSetting("resolution")).setValue(resolution);
        ((ListPreference)findSetting("worldFilter")).setValue("bilinear");
        ((ListPreference)findSetting("uiFilter")).setValue("bilinear");
        ((ListPreference)findSetting("sharpness")).setValue("0");
        ((CheckBoxPreference)findSetting("deband")).setChecked(false);
        ((CheckBoxPreference)findSetting("crispText")).setChecked(false);
        Toast.makeText(this,"Минимальная графика установлена. Перед перезапуском сохраните матч.",Toast.LENGTH_LONG).show();
    }
    private int nativeValue(int index,int fallback) { return game == null ? preferences.getInt("last-game-"+index,fallback) : game[index]; }
    private void loupeSettings(PreferenceGroup controls) {
        toggle(controls,"loupe","Лупа при строительстве",false);
        integers(controls,"loupeSize","Размер лупы",new int[]{50,75,100,125,150,175,200},100,"%",-1);
        integers(controls,"loupeZoom","Приближение лупы",new int[]{150,200,250,300,350,400},200,"%",-1);
    }
    private void strongholdResolutionLabels() {
        ListPreference item=(ListPreference)findSetting("resolution");
        android.graphics.Point size=new android.graphics.Point();getWindowManager().getDefaultDisplay().getRealSize(size);
        size.set(Math.max(size.x,size.y),Math.min(size.x,size.y));
        boolean wide=preferences.getBoolean("wide",true);
        CharSequence[] values=item.getEntryValues();String[] labels=new String[values.length];
        for(int i=0;i<values.length;i++) {
            int[] frame=GameActivity.strongholdMode(values[i].toString(),size.x,size.y,wide);
            labels[i]=frame[1]+" строк · "+frame[0]+" × "+frame[1]+(frame[1]==720?" (рекомендуется)":frame[1]==600?" (минимальное)":"");
        }
        item.setEntries(labels);item.setSummary("%s");
    }
    private void resolutionLabels() {
        ListPreference item=(ListPreference)findSetting("resolution");
        android.graphics.Point size=new android.graphics.Point();getWindowManager().getDefaultDisplay().getRealSize(size);
        size.set(Math.max(size.x,size.y),Math.min(size.x,size.y));
        float density=getResources().getDisplayMetrics().density;boolean wide=preferences.getBoolean("wide",true);
        CharSequence[] values=item.getEntryValues();String[] labels=new String[values.length];
        for(int i=0;i<values.length;i++) {
            String value=values[i].toString();
            int[] pixels=TouchControls.resolution(value,size.x,size.y,density,wide);
            String prefix=value.equals("auto")?"Авто · ":value.equals("minimum")?"Минимальное · ":value.equals("device")?"Устройство · ":"";
            labels[i]=prefix+pixels[0]+" × "+pixels[1];
        }
        item.setEntries(labels);item.setSummary("%s");
    }
    /* The single 0.8 "imageFilter" becomes both layer filters: a deliberate
     * pixel or bilinear choice is kept, the former default bicubic moves to FSR. */
    static void migrate(SharedPreferences p) {
        if ("autoplus".equals(p.getString("resolution","auto"))) p.edit().putString("resolution","auto").commit();
        if (!p.contains("imageFilter")) return;
        String old=p.getString("imageFilter","bicubic"),kept="pixels".equals(old)||"bilinear".equals(old)?old:"fsr";
        SharedPreferences.Editor edit=p.edit().remove("imageFilter");
        if (!p.contains("worldFilter")) edit.putString("worldFilter",kept);
        if (!p.contains("uiFilter")) edit.putString("uiFilter",kept);
        edit.commit();
    }
    private PreferenceCategory category(PreferenceGroup parent, String title) {
        PreferenceCategory group=new PreferenceCategory(this);group.setTitle(title);parent.addPreference(group);return group;
    }
    private interface Change { void set(String value); }
    private void list(PreferenceGroup group,String key,String title,String[] labels,String[] values,String current,Change change) {
        ListPreference item=new ListPreference(this);item.setKey(key);item.setTitle(title);item.setPersistent(false);
        item.setEntries(labels);item.setEntryValues(values);item.setValue(current);item.setSummary("%s");group.addPreference(item);
        item.setOnPreferenceChangeListener((preference,value) -> {
            String selected=(String)value;change.set(selected);item.setValue(selected);return true;
        });
    }
    private void integers(PreferenceGroup group,String key,String title,int[] numbers,int fallback,String suffix,int index) {
        int current=preferences.getInt(key,fallback);
        java.util.TreeSet<Integer> choices=new java.util.TreeSet<>();for(int n:numbers)choices.add(n);choices.add(current);
        String[] labels=new String[choices.size()],values=new String[choices.size()];int at=0;
        for(int n:choices){values[at]=Integer.toString(n);labels[at]=(key.equals("sharpness")&&n==0?"Выкл":n+suffix);at++;}
        list(group,key,title,labels,values,Integer.toString(current),value -> {
            int number=Integer.parseInt(value);preferences.edit().putInt(key,number).apply();
            if(index>=0)GameActivity.gameSetting(index,number);
        });
    }
    private void namedGame(PreferenceGroup group,int index,String title,String[] labels,int[] numbers,int fallback) {
        String key="game-"+index;String[] values=new String[numbers.length];for(int i=0;i<numbers.length;i++)values[i]=Integer.toString(numbers[i]);
        list(group,key,title,labels,values,Integer.toString(preferences.getInt(key,nativeValue(index,fallback))),value -> {
            int number=Integer.parseInt(value);preferences.edit().putInt(key,number).apply();GameActivity.gameSetting(index,number);
        });
    }
    private void toggle(PreferenceGroup group,String key,String title,boolean fallback) {
        CheckBoxPreference item=new CheckBoxPreference(this);item.setKey(key);item.setTitle(title);item.setPersistent(false);
        item.setChecked(preferences.getBoolean(key,fallback));group.addPreference(item);
        item.setOnPreferenceChangeListener((p,value) -> {
            preferences.edit().putBoolean(key,(Boolean)value).apply();
            if(key.equals("wide")){if(GameFiles.original(edition()))strongholdResolutionLabels();else resolutionLabels();}
            return true;
        });
    }
    private void gameToggle(PreferenceGroup group,int index,String title,boolean fallback) {
        String key="game-"+index;CheckBoxPreference item=new CheckBoxPreference(this);item.setKey(key);item.setTitle(title);item.setPersistent(false);
        item.setChecked(preferences.getInt(key,nativeValue(index,fallback?1:0))!=0);group.addPreference(item);
        item.setOnPreferenceChangeListener((p,value) -> {
            int number=(Boolean)value?1:0;preferences.edit().putInt(key,number).apply();GameActivity.gameSetting(index,number);return true;
        });
    }
    private void floatChoices(PreferenceGroup group,String key,String title,float[] numbers,float fallback,String suffix) {
        float current=preferences.getFloat(key,fallback);java.util.TreeSet<Float> choices=new java.util.TreeSet<>();
        for(float n:numbers)choices.add(n);choices.add(current);
        String[] labels=new String[choices.size()],values=new String[choices.size()];int at=0;
        for(float n:choices){values[at]=Float.toString(n);labels[at]=key.equals("opacity")?Math.round(n*100)+"%":n+suffix;at++;}
        list(group,key,title,labels,values,Float.toString(current),value -> preferences.edit().putFloat(key,Float.parseFloat(value)).apply());
    }
    private PreferenceScreen newSection(String key, String title, String summary) {
        PreferenceScreen page = getPreferenceManager().createPreferenceScreen(this);
        Preference link = new Preference(this); link.setKey("section-" + key); link.setTitle(title); link.setSummary(summary);
        link.setOnPreferenceClickListener(item -> { showSection(key); return true; }); home.addPreference(link);
        return page;
    }
    private void showSection(String key) {
        PreferenceScreen page = "graphics".equals(key) ? graphics : "audio".equals(key) ? audio : "game".equals(key) ? gameplay : home;
        section = page == home ? "" : key;
        setPreferenceScreen(page);
        setTitle((page == graphics ? "Графика" : page == audio ? "Звук" : page == gameplay ? "Игра" : "Настройки") + " · " + GameFiles.title(edition()));
        getListView().setSelection(0);
    }
    private Preference findSetting(String key) {
        for (PreferenceScreen page : new PreferenceScreen[]{graphics, audio, gameplay}) {
            Preference item = page.findPreference(key); if (item != null) return item;
        }
        return null;
    }
    private void resetSettings() {
        Preference reset = new Preference(this); reset.setTitle("Сбросить настройки");
        reset.setSummary("Графика, звук, игра, управление и расположение кнопок этой редакции"); home.addPreference(reset);
        reset.setOnPreferenceClickListener(item -> {
            new AlertDialog.Builder(this).setTitle("Сбросить настройки?")
                    .setMessage("Восстановить исходные настройки графики, звука, игры и управления для «" + GameFiles.title(edition()) + "»? Расположение кнопок тоже будет сброшено. Сохранения и папка игры останутся.")
                    .setNegativeButton("Отмена", null).setPositiveButton("Сбросить", (dialog, which) -> {
                        SharedPreferences.Editor edit = preferences.edit();
                        clearSettings(graphics, edit); clearSettings(audio, edit); clearSettings(gameplay, edit);
                        for (String key : preferences.getAll().keySet())
                            if (key.startsWith("pos-") || key.startsWith("control-") || key.equals("extra")) edit.remove(key);
                        edit.remove("imageFilter").remove("layoutEditRequest").remove("groupWheel").remove("markerWheel");
                        game = new int[]{40,1,0,2,1,90,80,85,1};
                        for (int i=0;i<(GameFiles.original(edition())?8:9);i++) {
                            edit.putInt("game-"+i,game[i]).putInt("last-game-"+i,game[i]);
                            GameActivity.gameSetting(i,game[i]);
                        }
                        edit.commit(); buildScreens(); showSection("");
                        Toast.makeText(this,"Исходные настройки восстановлены. Разрешение — после перезапуска игры.",Toast.LENGTH_LONG).show();
                    }).show();
            return true;
        });
    }
    private void clearSettings(PreferenceGroup group, SharedPreferences.Editor edit) {
        for (int i=0;i<group.getPreferenceCount();i++) {
            Preference item=group.getPreference(i);
            if (item.getKey()!=null) edit.remove(item.getKey());
            if (item instanceof PreferenceGroup) clearSettings((PreferenceGroup)item,edit);
        }
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state); state.putString("section", section);
    }
    @Override public void onBackPressed() { if (!section.isEmpty()) showSection(""); else super.onBackPressed(); }
    @Override public boolean onOptionsItemSelected(android.view.MenuItem item) {
        if(item.getItemId()==android.R.id.home){onBackPressed();return true;}return super.onOptionsItemSelected(item);
    }
}
