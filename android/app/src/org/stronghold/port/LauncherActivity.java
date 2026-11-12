package org.stronghold.port;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONObject;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** The launcher never loads JNI; each game process loads exactly one edition. */
public final class LauncherActivity extends Activity {
    private static final int PICK_GAME_FOLDER = 11, PICK_ARCHIVE = 13, PICK_MOD = 14;
    /* Tile order: Crusader, Crusader Extreme, the original Stronghold HD. */
    private static final String[] EDITIONS = GameFiles.EDITIONS;
    private static final String EXTERNAL_STORAGE = "com.android.externalstorage.documents";
    static boolean largeAddressAware(Context context) {
        if (!android.os.Process.is64Bit()) return false;
        android.app.ActivityManager manager = (android.app.ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        android.app.ActivityManager.MemoryInfo memory = new android.app.ActivityManager.MemoryInfo();
        if (manager != null) manager.getMemoryInfo(memory);
        return context.getSharedPreferences("LauncherActivity", Context.MODE_PRIVATE)
            .getBoolean("large_address_aware", memory.totalMem >= (5L << 30));
    }

    private SharedPreferences preferences;
    private LinearLayout setupActions;
    private Button changeFiles;
    private TextView filesHint;
    private boolean showingLauncher, selectingFiles, pickingGog;
    private Switch skip;
    private TextView status;
    private TextView filesHeading, ucpStatus, modStatus;
    private TextView diagnosticNote;
    private Button diagnosticButton;
    private boolean copyingDiagnostics;
    private final Map<TextView, LinearLayout> recoveryActions = new HashMap<>();
    private final View[] gameRecovery = new View[3];
    private LinearLayout fileDetails, fileProgress, modProgress;
    private boolean filesExpanded;
    private final TextView[] gameStatus = new TextView[3];
    private final TextView[] gameNotes = new TextView[3];
    private final LinearLayout[] gameCards = new LinearLayout[3];
    private Button access;
    private final Button[] play = new Button[3];
    private int scanVersion;
    private boolean checking;
    private volatile boolean transferCancelled;
    private volatile Process gogProcess;
    private String transferNotice;
    private String modEdition;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        I18n.apply(this);
        Theme.edgeToEdge(this);
        preferences = getSharedPreferences("LauncherActivity", MODE_PRIVATE);
        if (state != null) {
            modEdition = state.getString("modEdition");
            filesExpanded = state.getBoolean("filesExpanded");
        }
        handleIntent(getIntent());
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state); state.putString("modEdition", modEdition);
        state.putBoolean("filesExpanded", filesExpanded);
    }
    @Override public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        if (showingLauncher) showLauncher(null);
    }
    private static String pathKey(String edition) { return GameFiles.pathKey(edition); }
    private String[] savedPaths() {
        return GameFiles.savedPaths(preferences.getString("game", null), preferences.getString("extremeGame", null),
                preferences.getString("strongholdGame", null));
    }
    private String gamePath(String edition) {
        String[] paths = savedPaths();
        for (int i = 0; i < EDITIONS.length; i++) if (EDITIONS[i].equals(edition)) return paths[i];
        return GameFiles.DEFAULT_GAME;
    }
    private File filesFolder() {
        return new File(preferences.getString("filesFolder", new File(Environment.getExternalStorageDirectory(), "Stronghold").getAbsolutePath()));
    }
    static Class<? extends SettingsActivity> settingsClass(String edition) {
        return GameFiles.original(edition) ? StrongholdSettingsActivity.class : SettingsActivity.settingsClass(edition);
    }
    private int dp(float value) { return Theme.dp(this, value); }
    private void gap(LinearLayout parent, int height) { parent.addView(new View(this), new LinearLayout.LayoutParams(1, dp(height))); }
    private TextView heading(LinearLayout parent, String text) {
        TextView view = Theme.text(this, text, 17, Theme.TEXT, true); parent.addView(view); return view;
    }
    private TextView note(LinearLayout parent, String text) {
        TextView view = Theme.text(this, text, 13, Theme.MUTED, false); view.setPadding(0, dp(4), 0, dp(6));
        parent.addView(view); return view;
    }
    private void showLauncher(String error) {
        showingLauncher = true;
        recoveryActions.clear();
        FrameLayout frame = new FrameLayout(this);
        ScrollView scroll = new ScrollView(this); scroll.setFillViewport(true); scroll.setClipToPadding(false);
        LinearLayout page = new LinearLayout(this); page.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(page, new FrameLayout.LayoutParams(-1, -2)); frame.addView(scroll, new FrameLayout.LayoutParams(-1, -1));

        boolean wide = getResources().getConfiguration().screenWidthDp >= 600;
        LinearLayout header = Theme.row(this);
        ImageView logo = new ImageView(this); logo.setImageResource(resource("ic_stronghold", "mipmap"));
        header.addView(logo, new LinearLayout.LayoutParams(dp(52), dp(52)));
        LinearLayout titles = new LinearLayout(this); titles.setOrientation(LinearLayout.VERTICAL); titles.setPadding(dp(14), 0, 0, 0);
        titles.addView(Theme.text(this, "Stronghold Launcher", 25, Theme.ACCENT, true));
        titles.addView(Theme.text(this, I18n.t("Stronghold HD, Crusader и Crusader Extreme · Android ARM64"), 13, Theme.MUTED, false));
        header.addView(titles, new LinearLayout.LayoutParams(0, -2, 1));
        access = Theme.button(this, "", false); access.setOnClickListener(v -> openStorageSettings());
        if (wide) header.addView(access, new LinearLayout.LayoutParams(-2, -2));
        page.addView(header);
        if (!wide) { gap(page, 8); page.addView(access, new LinearLayout.LayoutParams(-1, -2)); }
        gap(page, 14);

        LinearLayout sourceCard = Theme.card(this);
        filesHeading = Theme.text(this, "", 16, Theme.ACCENT, true);
        filesHeading.setMinHeight(dp(48));
        filesHeading.setFocusable(true);
        filesHeading.setOnClickListener(v -> { filesExpanded = !filesExpanded; foldFiles(); });
        sourceCard.addView(filesHeading);
        LinearLayout source = new LinearLayout(this);
        source.setOrientation(LinearLayout.VERTICAL);
        fileDetails = source;
        sourceCard.addView(source);
        filesHint = note(source, I18n.t("Проверка установленных игр…"));
        setupActions = Theme.row(this);
        Button choose = Theme.button(this, I18n.t("Выбрать папку…"), false);
        choose.setOnClickListener(v -> { if (!requestStorage()) pickFolder(); });
        Button archive = Theme.button(this, I18n.t("Распаковать ZIP…"), false);
        archive.setOnClickListener(v -> { if (!requestStorage()) pickArchive(); });
        setupActions.addView(choose, Theme.weight(this, 1, 0)); setupActions.addView(archive, Theme.weight(this, 1, 8));
        source.addView(setupActions);
        Button gog = Theme.button(this, I18n.t("Установить из GOG…"), false);
        gog.setOnClickListener(v -> { if (!requestStorage()) { pickingGog = true; pickFolder(); } });
        source.addView(gog, new LinearLayout.LayoutParams(-1, -2));
        changeFiles = Theme.button(this, I18n.t("Изменить файлы игр…"), false);
        changeFiles.setVisibility(View.GONE); changeFiles.setOnClickListener(v -> fileActions());
        source.addView(changeFiles, new LinearLayout.LayoutParams(-1, -2));
        status = inlineStatus(sourceCard);
        status.setVisibility(View.GONE);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        if (fileProgress == null) {
            fileProgress = new LinearLayout(this);
            fileProgress.setOrientation(LinearLayout.VERTICAL);
            fileProgress.setVisibility(View.GONE);
        }
        attach(sourceCard, fileProgress);
        foldFiles();
        page.addView(sourceCard);

        LinearLayout columns = new LinearLayout(this); columns.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        LinearLayout games = new LinearLayout(this), files = new LinearLayout(this);
        games.setOrientation(LinearLayout.VERTICAL); files.setOrientation(LinearLayout.VERTICAL);
        columns.addView(games, wide ? Theme.weight(this, 1, 0) : new LinearLayout.LayoutParams(-1, -2));
        columns.addView(files, wide ? Theme.weight(this, 1.15f, 14) : new LinearLayout.LayoutParams(-1, -2));
        page.addView(columns);
        for (int i = 0; i < EDITIONS.length; i++) games.addView(gameTile(i));

        LinearLayout options = Theme.card(this);
        heading(options, I18n.t("Запуск"));
        skip = new Switch(this); skip.setText(I18n.t("Пропускать ролики, рекламу и предупреждение об онлайне"));
        skip.setTextColor(Theme.TEXT); skip.setChecked(preferences.getBoolean("skipMovies", false)); skip.setPadding(0, dp(6), 0, dp(6));
        skip.setOnCheckedChangeListener((button, checked) -> preferences.edit().putBoolean("skipMovies", checked).apply());
        options.addView(skip);
        if (android.os.Process.is64Bit()) {
            Switch memory = new Switch(this);
            memory.setText(I18n.t("Лимит памяти игры до 4 ГБ"));
            memory.setTextColor(Theme.TEXT); memory.setChecked(largeAddressAware(this));
            memory.setPadding(0, dp(6), 0, dp(6));
            memory.setOnCheckedChangeListener((button, checked) -> preferences.edit().putBoolean("large_address_aware", checked).apply());
            options.addView(memory);
        }
        note(options, I18n.t("Параметры запуска применяются при новом запуске процесса игры. Перед перезапуском сохраните матч."));
        files.addView(options);
        Button language = Theme.button(this, I18n.t("Язык интерфейса…"), false);
        language.setOnClickListener(v -> I18n.choose(this, () -> showLauncher(null)));
        options.addView(language);

        LinearLayout ucp = Theme.card(this);
        heading(ucp, "Unofficial Crusader Patch");
        Button github = Theme.button(this, I18n.t("UCP на GitHub"), false);
        github.setOnClickListener(v -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(UcpOptions.WEBSITE))); }
            catch (ActivityNotFoundException unavailable) { setStatus(ucpStatus, UcpOptions.WEBSITE, false); }
        });
        ucp.addView(github);
        Button disable = Theme.button(this, I18n.t("Отключить все функции UCP"), false);
        disable.setOnClickListener(v -> {
            modStatus = ucpStatus;
            for (String game : EDITIONS) if (running(game)) {
                modProblem(new IOException(I18n.t("Завершите запущенную игру перед изменением модов. Новые настройки применяются при следующем запуске."))); return;
            }
            try {
                String revision = modCatalog().getJSONObject("upstream").getString("framework_revision");
                for (String game : new String[]{"classic", "extreme"})
                    GameMods.configureFeatures(UcpOptions.config(this, game), revision, java.util.Collections.emptySet());
                setStatus(ucpStatus, I18n.t("Все функции UCP отключены. Пакеты данных и локализации сохранены."), false);
            } catch (Exception failure) { modProblem(failure); }
        });
        ucp.addView(disable); files.addView(ucp);
        ucpStatus = inlineStatus(ucp);
        ucpStatus.setVisibility(View.GONE);
        if (error != null) setStatus(gameNotes[editionIndex(preferences.getString("edition", "extreme"))], error, true);

        LinearLayout diagnostics = Theme.card(this);
        heading(diagnostics, I18n.t("Диагностика"));
        diagnosticButton = Theme.button(this, I18n.t("Скопировать отчёт диагностики"), false);
        diagnosticButton.setOnClickListener(v -> copyDiagnostics());
        Theme.enable(diagnosticButton, !copyingDiagnostics);
        diagnostics.addView(diagnosticButton);
        diagnosticNote = inlineStatus(diagnostics);
        files.addView(diagnostics);

        frame.setOnApplyWindowInsetsListener((view, insets) -> Theme.padInsets(page, insets, dp(20), dp(14), dp(20), dp(20)));
        setContentView(frame);
        updateStorageAccess();
        detectGames();
    }
    private int resource(String name, String type) { return getResources().getIdentifier(name, type, getPackageName()); }
    private View gameTile(int index) {
        final String item = EDITIONS[index];
        LinearLayout card = Theme.card(this);
        gameCards[index] = card;
        LinearLayout top = Theme.row(this);
        ImageView icon = new ImageView(this); icon.setImageResource(resource(index == 0 ? "ic_classic" : index == 1 ? "ic_extreme" : "ic_stronghold", "mipmap"));
        top.addView(icon, new LinearLayout.LayoutParams(dp(56), dp(56)));
        LinearLayout titles = new LinearLayout(this); titles.setOrientation(LinearLayout.VERTICAL); titles.setPadding(dp(12), 0, 0, 0);
        titles.addView(Theme.text(this, index == 0 ? "Stronghold Crusader" : index == 1 ? "Crusader Extreme" : "Stronghold HD", 19, Theme.TEXT, true));
        gameStatus[index] = Theme.text(this, I18n.t("Проверка файлов…"), 13, Theme.MUTED, false);
        titles.addView(gameStatus[index]);
        top.addView(titles, new LinearLayout.LayoutParams(0, -2, 1));
        card.addView(top);
        gameNotes[index] = inlineStatus(card);
        gameNotes[index].setVisibility(View.GONE);
        gameNotes[index].setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        if (item.equals(modEdition)) {
            modStatus = gameNotes[index];
            if (modProgress != null) attach(card, modProgress);
        }
        gap(card, 10);
        LinearLayout actions = Theme.row(this);
        Button button = Theme.button(this, I18n.t("Играть"), true, item); play[index] = button; Theme.enable(button, false);
        button.setOnClickListener(v -> attempt(item, gamePath(item), skip.isChecked()));
        Button settings = Theme.button(this, I18n.t("Настройки"), false);
        settings.setOnClickListener(v -> startActivity(new Intent(this, settingsClass(item))));
        actions.addView(button, Theme.weight(this, 1.3f, 0)); actions.addView(settings, Theme.weight(this, 1, 8));
        card.addView(actions);
        LinearLayout repair = Theme.row(this);
        Button folder = Theme.button(this, I18n.t("Выбрать папку…"), false);
        folder.setOnClickListener(v -> { if (!checking && !requestStorage()) { pickingGog = false; pickFolder(); } });
        Button retry = Theme.button(this, I18n.t("Проверить снова"), false);
        retry.setOnClickListener(v -> { if (!checking && !requestStorage()) detectGames(); });
        repair.addView(folder, Theme.weight(this, 1, 0));
        repair.addView(retry, Theme.weight(this, 1, 8));
        repair.setVisibility(View.GONE);
        gameRecovery[index] = repair;
        card.addView(repair);
        if (!GameFiles.original(item)) {
            Button mods = Theme.button(this, I18n.t("Моды…"), false);
            mods.setOnClickListener(v -> modsMenu(item));
            card.addView(mods, new LinearLayout.LayoutParams(-1, -2));
        }
        return card;
    }
    private File modsFolder() { return new File(getFilesDir(), "mods"); }
    private Set<String> modSelection(String key, String edition) throws IOException {
        if (key.equals("ucp-")) return GameMods.features(UcpOptions.config(this, edition));
        return new HashSet<>(preferences.getStringSet(key + edition, java.util.Collections.emptySet()));
    }
    private JSONObject modCatalog() throws Exception { return UcpOptions.catalog(this); }
    private void modProblem(Exception error) {
        setStatus(modStatus != null ? modStatus : status, error.getMessage(), true);
    }
    private void modsMenu(String edition) {
        if (GameFiles.original(edition)) return;
        if (checking) return;
        modStatus = gameNotes[editionIndex(edition)];
        for (String game : EDITIONS) if (running(game)) {
            modProblem(new IOException(I18n.t("Завершите запущенную игру перед изменением модов. Новые настройки применяются при следующем запуске.")));
            return;
        }
        try {
            JSONObject catalog = modCatalog();
            String revision = catalog.getJSONObject("upstream").getString("framework_revision");
            new AlertDialog.Builder(this).setTitle(I18n.t("Моды · ") + GameFiles.title(edition))
                    .setItems(new String[] {I18n.t("Функции Unofficial Crusader Patch"), I18n.t("Пакеты данных и локализации"), I18n.t("Версия и совместимость")}, (d, item) -> {
                        if (item == 0) ucpOptions(edition, catalog);
                        else if (item == 1) assetMods(edition, catalog);
                        else new AlertDialog.Builder(this).setTitle("Unofficial Crusader Patch")
                                .setMessage(I18n.t("Зафиксированная версия UCP3:\n") + revision + I18n.t("\n\nAndroid использует подготовленные функции UCP2 Legacy и пакеты данных. Поддержка каждой функции указана в списке. Windows DLL, ASI и произвольные Lua-плагины требуют отдельной адаптации.\n\nОригинальные файлы игры сохраняются. При совпадении файлов приоритет имеет первый включённый пакет в списке."))
                                .setPositiveButton("OK", null).show();
                    }).setNegativeButton(I18n.t("Закрыть"), null).show();
        } catch (Exception error) { modProblem(error); }
    }
    private void saveMods(String edition, JSONObject catalog, Set<String> features, Set<String> packs) throws Exception {
        GameMods.configure(new File(getFilesDir(), "mods-" + edition + ".cfg"), catalog.getJSONObject("upstream").getString("framework_revision"),
                features, GameMods.packs(modsFolder(), edition), packs);
        preferences.edit().putStringSet("ucp-"+edition, features).putStringSet("packs-"+edition, packs).apply();
        detectGames();
    }
    private String gameLanguage(File game, String edition) {
        try {
            Set<String> selected = modSelection("packs-", edition);
            String name = GameFiles.original(edition) ? "sh.tex" : "cr.tex";
            for (GameMods.Pack pack : GameMods.packs(modsFolder(), edition)) if (selected.contains(pack.id)) {
                File[] files = pack.assets().listFiles();
                if (files != null) for (File file : files) if (file.isFile() && file.getName().equalsIgnoreCase(name)) return GameFiles.languageFile(file);
            }
            return GameFiles.language(game, edition);
        } catch (IOException unavailable) { return I18n.t("Не удалось проверить язык мода"); }
    }
    private void ucpOptions(String edition, JSONObject catalog) { UcpOptions.show(this, edition, false, this::detectGames); }
    private void assetMods(String edition, JSONObject catalog) {
        try {
            List<GameMods.Pack> packs = GameMods.packs(modsFolder(), edition); Set<String> selected = modSelection("packs-", edition);
            String[] names = new String[packs.size()]; boolean[] checked = new boolean[packs.size()];
            for (int n=0; n<names.length; n++) { names[n] = packs.get(n).name; checked[n] = selected.contains(packs.get(n).id); }
            AlertDialog dialog = new AlertDialog.Builder(this).setTitle(I18n.t("Пакеты · удерживайте для удаления"))
                    .setMultiChoiceItems(names, checked, (d, item, enabled) -> { if (enabled) selected.add(packs.get(item).id); else selected.remove(packs.get(item).id); })
                    .setPositiveButton(I18n.t("Применить"), (d, which) -> { try { saveMods(edition, catalog, modSelection("ucp-", edition), selected); } catch (Exception error) { modProblem(error); } })
                    .setNegativeButton(I18n.t("Отмена"), null).setNeutralButton(I18n.t("Установить ZIP…"), (d, which) -> {
                        modEdition = edition;
                        try { startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE), PICK_MOD); }
                        catch (ActivityNotFoundException error) { modProblem(error); }
                    }).show();
            dialog.getListView().setOnItemLongClickListener((list, view, item, id) -> {
                GameMods.Pack pack = packs.get(item);
                new AlertDialog.Builder(this).setTitle(I18n.t("Удалить «")+pack.name+"»?").setMessage(I18n.t("Файлы игры и сохранения сохранятся."))
                        .setPositiveButton(I18n.t("Удалить"), (d, which) -> {
                            try {
                                for (String game : EDITIONS) {
                                    Set<String> enabled = modSelection("packs-", game); enabled.remove(pack.id);
                                    saveMods(game, catalog, modSelection("ucp-", game), enabled);
                                }
                                GameMods.delete(modsFolder(), pack.folder); dialog.dismiss(); assetMods(edition, catalog);
                            } catch (Exception error) { modProblem(error); }
                        }).setNegativeButton(I18n.t("Отмена"), null).show(); return true;
            });
        } catch (Exception error) { modProblem(error); }
    }
    private void installMod(Uri uri) {
        if (checking || modEdition == null) return;
        String edition = modEdition; checking = true; transferCancelled = false;
        for (Button button : play) Theme.enable(button, false);
        modStatus = gameNotes[editionIndex(edition)];
        if (modProgress == null) {
            modProgress = new LinearLayout(this); modProgress.setOrientation(LinearLayout.VERTICAL);
        }
        attach(gameCards[editionIndex(edition)], modProgress);
        setStatus(modStatus, null, false);
        Progress progress = showProgress(modProgress, I18n.t("Установка мода…"), () -> transferCancelled = true);
        new Thread(() -> {
            Exception error = null; GameMods.Pack pack = null;
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException(I18n.t("Архив недоступен"));
                long total = documentSize(uri);
                pack = GameMods.install(new Counting(input, bytes -> progress.update(bytes, total, 0)), modsFolder(), () -> transferCancelled);
            } catch (Exception failure) { error = failure; }
            final Exception problem = error; final GameMods.Pack installed = pack;
            runOnUiThread(() -> {
                checking = false; modProgress.setVisibility(View.GONE);
                if (isFinishing() || isDestroyed()) return;
                detectGames();
                if (problem != null) modProblem(problem);
                else if (!installed.supports(edition)) setStatus(gameNotes[editionIndex(edition)],
                        "«"+installed.name+I18n.t("» доступен в списке модов для ")+installed.edition, false);
                else try { assetMods(edition, modCatalog()); } catch (Exception failure) { modProblem(failure); }
            });
        }, "mod-import").start();
    }
    private void fileActions() {
        if (checking) return;
        pickingGog = false;
        new AlertDialog.Builder(this).setTitle(I18n.t("Файлы игр"))
                .setItems(new String[] {I18n.t("Выбрать папку…"), I18n.t("Распаковать ZIP…"), I18n.t("Указать папку вручную…"), I18n.t("Проверить снова")}, (dialog, which) -> {
                    if (requestStorage()) return;
                    if (which == 0) pickFolder();
                    else if (which == 1) pickArchive();
                    else if (which == 2) manualFolder();
                    else detectGames();
                }).show();
    }
    private void manualFolder() {
        EditText folder = Theme.field(this, filesFolder().getAbsolutePath(), I18n.t("Папка игры или общая папка с играми"));
        new AlertDialog.Builder(this).setTitle(I18n.t("Папка с файлами игр")).setView(folder)
                .setPositiveButton(I18n.t("Проверить"), (dialog, which) -> selectFolder(new File(folder.getText().toString().trim()), ""))
                .setNegativeButton(I18n.t("Отмена"), null).show();
    }
    private void setStatus(String text, boolean problem) {
        setStatus(status, text, problem);
    }
    private void setStatus(TextView target, String text, boolean problem) {
        if (target == null) return;
        boolean unchanged = (text == null ? target.getVisibility() == View.GONE : text.contentEquals(target.getText()))
                && target.getCurrentTextColor() == (problem ? Theme.BAD : Theme.MUTED);
        target.setText(text); target.setTextColor(problem ? Theme.BAD : Theme.MUTED);
        target.setVisibility(text == null || text.isEmpty() ? View.GONE : View.VISIBLE);
        if (unchanged) return;
        LinearLayout actions = recoveryActions.get(target);
        if (actions == null) return;
        actions.removeAllViews();
        if (!problem || text == null || text.isEmpty()) return;
        preferences.edit().putString("last_error", text).apply();
        if (target == status) {
            Button folder = Theme.button(this, I18n.t("Выбрать папку…"), false);
            folder.setOnClickListener(v -> { if (!checking && !requestStorage()) { pickingGog = false; pickFolder(); } });
            Button archive = Theme.button(this, I18n.t("Распаковать ZIP…"), false);
            archive.setOnClickListener(v -> { if (!checking && !requestStorage()) pickArchive(); });
            actions.addView(folder); actions.addView(archive);
        } else if (target == diagnosticNote) {
            Button retry = Theme.button(this, I18n.t("Скопировать отчёт диагностики"), false);
            retry.setOnClickListener(v -> copyDiagnostics()); actions.addView(retry);
        }
    }

    private TextView inlineStatus(LinearLayout parent) {
        TextView view = note(parent, ""); view.setVisibility(View.GONE);
        view.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        LinearLayout actions = new LinearLayout(this); actions.setOrientation(LinearLayout.VERTICAL);
        parent.addView(actions); recoveryActions.put(view, actions);
        return view;
    }

    private void showLaunchError(String edition, String text) {
        if (!showingLauncher) { showLauncher(text); return; }
        setStatus(gameNotes[editionIndex(edition)], text, true);
        detectGames();
    }

    private void copyDiagnostics() {
        if (copyingDiagnostics) return;
        copyingDiagnostics = true; Theme.enable(diagnosticButton, false);
        setStatus(diagnosticNote, I18n.t("Подготовка отчёта…"), false);
        String edition = preferences.getString("edition", "extreme"), error = preferences.getString("last_error", "");
        new Thread(() -> {
            String gpuText;
            try { Gpu found = Gpu.get(this); gpuText = found.renderer + " (" + found.vendor + ")"; }
            catch (RuntimeException | LinkageError failure) { gpuText = "unavailable: " + failure; }
            String report = Diagnostics.report(this, GameFiles.title(edition), gpuText, "native 2D", "original 2D assets", error);
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                copyingDiagnostics = false; Theme.enable(diagnosticButton, true);
                boolean copied = Diagnostics.copy(this, report);
                setStatus(diagnosticNote, I18n.t(copied ? "Отчёт диагностики скопирован" : "Не удалось скопировать отчёт"), !copied);
            });
        }, "launcher-diagnostics").start();
    }
    private int editionIndex(String edition) {
        for (int i = 0; i < EDITIONS.length; ++i) if (EDITIONS[i].equals(edition)) return i;
        return 1;
    }
    private void foldFiles() {
        filesHeading.setText((filesExpanded ? "▾ " : "▸ ") + I18n.t("Файлы игр"));
        fileDetails.setVisibility(filesExpanded && fileProgress.getVisibility() != View.VISIBLE ? View.VISIBLE : View.GONE);
    }
    private static void attach(LinearLayout parent, View view) {
        if (view.getParent() != null) ((android.view.ViewGroup) view.getParent()).removeView(view);
        parent.addView(view);
    }
    private void detectGames() {
        if (selectingFiles) return; // Returning from the picker must not cancel its pending validation.
        scanGames(new File(Environment.getExternalStorageDirectory(), "Stronghold"), false, null);
    }
    private void selectFolder(File folder, String notice) {
        if (pickingGog) { pickingGog = false; chooseGog(folder); return; }
        preferences.edit().putString("filesFolder", folder.getAbsolutePath()).apply();
        scanGames(folder, true, I18n.t("Файлы игр в ") + folder + (notice.isEmpty() ? "" : "\n" + notice));
    }
    private void scanGames(File source, boolean replaceMatching, String notice) {
        if (!showingLauncher) return;
        final int scan = ++scanVersion;
        selectingFiles = replaceMatching;
        final String[] saved = savedPaths();
        new Thread(() -> {
            GameFiles.Scan result = GameFiles.scan(saved, source, replaceMatching, true);
            String[] languages = new String[EDITIONS.length];
            for (int i = 0; i < languages.length; i++) if (result.problems[i] == null)
                languages[i] = gameLanguage(new File(result.paths[i]), EDITIONS[i]);
            runOnUiThread(() -> {
                if (scan != scanVersion || !showingLauncher || isFinishing() || isDestroyed()) return;
                selectingFiles = false;
                SharedPreferences.Editor settings = preferences.edit();
                StringBuilder missing = new StringBuilder();
                for (int i = 0; i < EDITIONS.length; i++) {
                    String edition = EDITIONS[i], problem = result.problems[i], warning = result.warnings[i];
                    boolean valid = problem == null;
                    if (valid) settings.putString(pathKey(edition), result.paths[i]);
                    else { if (missing.length() > 0) missing.append(", "); missing.append(GameFiles.title(edition)); }
                    gameStatus[i].setText(valid ? warning == null ? I18n.t("Готов к запуску · ") + languages[i] : warning : problem);
                    gameStatus[i].setTextColor(valid ? warning == null ? Theme.GOOD : Theme.ACCENT : Theme.BAD);
                    Theme.enable(play[i], valid);
                    gameRecovery[i].setVisibility(valid ? View.GONE : View.VISIBLE);
                }
                settings.apply();
                boolean complete = result.complete();
                setupActions.setVisibility(complete ? View.GONE : View.VISIBLE);
                changeFiles.setVisibility(complete ? View.VISIBLE : View.GONE);
                filesHint.setText(complete ? I18n.t("Все три игры найдены и проверены.")
                        : I18n.t("Добавьте файлы: ") + missing + I18n.t(". Папка или ZIP распознаются автоматически; можно выбрать общую папку с играми."));
                if (notice != null) setStatus(result.discoveryProblem == null ? notice : result.discoveryProblem, result.discoveryProblem != null);
            });
        }, "game-discovery").start();
    }
    private boolean requestStorage() {
        if (storageAllowed()) return false;
        openStorageSettings();
        return true;
    }
    private void updateStorageAccess() {
        if (access == null) return;
        boolean allowed = storageAllowed();
        access.setText(I18n.t("Разрешить доступ к файлам"));
        access.setVisibility(allowed ? View.GONE : View.VISIBLE);
    }
    private void openStorageSettings() {
        if (android.os.Build.VERSION.SDK_INT >= 30)
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        else requestPermissions(new String[] {android.Manifest.permission.READ_EXTERNAL_STORAGE,
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, 12);
    }
    private boolean storageAllowed() {
        return android.os.Build.VERSION.SDK_INT >= 30 ? Environment.isExternalStorageManager() :
                checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }
    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == 12) { updateStorageAccess(); if (showingLauncher) detectGames(); }
    }

    /* ---- Storage Access Framework: the system file picker chooses folders and archives. ---- */
    private void pickFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        Uri initial = documentUri(filesFolder());
        if (initial != null) intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, initial);
        try { startActivityForResult(intent, PICK_GAME_FOLDER); }
        catch (ActivityNotFoundException missing) { setStatus(I18n.t("Системный выбор папок недоступен на этом устройстве."), true); manualFolder(); }
    }
    private void pickArchive() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/x-zip", "application/octet-stream"});
        try { startActivityForResult(intent, PICK_ARCHIVE); }
        catch (ActivityNotFoundException missing) { setStatus(I18n.t("Системный выбор файлов недоступен на этом устройстве."), true); }
    }
    private void chooseGog(File folder) {
        File[] installers = folder.listFiles(file -> file.isFile() && file.getName().toLowerCase(Locale.ROOT).startsWith("setup_") &&
                file.getName().toLowerCase(Locale.ROOT).endsWith(".exe"));
        if (installers == null || installers.length == 0) { setStatus(I18n.t("В папке не найдены установщики setup_*.exe. Все BIN должны лежать рядом с EXE."), true); return; }
        java.util.Arrays.sort(installers, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        String[] names = new String[installers.length];
        for (int i = 0; i < names.length; i++) names[i] = installers[i].getName();
        new AlertDialog.Builder(this).setTitle(I18n.t("Установщик GOG · выберите язык"))
                .setItems(names, (dialog, which) -> inspectGog(installers[which])).setNegativeButton(I18n.t("Отмена"), null).show();
    }
    private void inspectGog(File installer) {
        if (checking) return;
        checking = true; setStatus(I18n.t("Чтение установщика GOG…"), false);
        new Thread(() -> {
            GogInstaller.Info info = null; String error = null;
            try { info = GogInstaller.inspect(this, installer); }
            catch (IOException failure) { error = failure.getMessage(); }
            final GogInstaller.Info found = info; final String problem = error;
            runOnUiThread(() -> {
                checking = false;
                if (isFinishing() || isDestroyed()) return;
                if (problem != null) { setStatus(problem, true); return; }
                File target = freshFolder("gog-" + found.game);
                String text = ("stronghold".equals(found.game) ? "Stronghold HD" : "Stronghold Crusader / Extreme") +
                        I18n.t("\nФайлов игры: ") + found.files + I18n.t(". После установки: ") + megabytes(found.bytes) +
                        I18n.t(".\nСжатых блоков: ") + found.compressed + I18n.t(" из ") + found.chunks + "." +
                        (found.split ? I18n.t("\nДля установки нужны EXE и все BIN из той же папки.") : "") +
                        I18n.t("\n\nПапка установки: ") + target + I18n.t("\nИсходный установщик сохраняется. Сохранения игры хранятся отдельно.");
                new AlertDialog.Builder(this).setTitle(I18n.t("Установить GOG")).setMessage(text)
                        .setPositiveButton(I18n.t("Установить"), (dialog, which) -> {
                            long began = System.nanoTime();
                            transfer(I18n.t("Установка GOG"), target, progress -> {
                                GogInstaller.extract(this, found, target, () -> transferCancelled, process -> {
                                    gogProcess = process; if (process != null && transferCancelled) process.destroyForcibly();
                                }, progress::update);
                                transferNotice = I18n.t("GOG установлен за ") + Math.round((System.nanoTime() - began) / 1e9) + I18n.t(" с. Язык: ") +
                                        GameFiles.language(target, "stronghold".equals(found.game) ? "stronghold" : "classic");
                            });
                        }).setNegativeButton(I18n.t("Отмена"), null).show();
            });
        }, "gog-inspect").start();
    }
    /* A shared-storage path the picker can open at; other volumes start at its default. */
    static Uri documentUri(File folder) {
        String root = Environment.getExternalStorageDirectory().getAbsolutePath(), value = folder.getAbsolutePath();
        while (folder != null && !folder.isDirectory()) { folder = folder.getParentFile(); value = folder == null ? "" : folder.getAbsolutePath(); }
        if (!value.equals(root) && !value.startsWith(root + "/")) return null;
        return DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE, "primary:" + value.substring(Math.min(value.length(), root.length() + 1)));
    }
    /* The direct path of a picked external-storage tree; null for other providers. */
    static File treePath(Uri tree) {
        String id;
        try { id = DocumentsContract.getTreeDocumentId(tree); } catch (IllegalArgumentException invalid) { return null; }
        if ("com.android.providers.downloads.documents".equals(tree.getAuthority()))
            return id.startsWith("raw:") ? new File(id.substring(4)) : null;
        if (!EXTERNAL_STORAGE.equals(tree.getAuthority())) return null;
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon), relative = id.substring(colon + 1);
        File base = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory()
                : "home".equalsIgnoreCase(volume) ? new File(Environment.getExternalStorageDirectory(), "Documents")
                : new File("/storage", volume);
        return relative.isEmpty() ? base : new File(base, relative);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null || !showingLauncher) {
            if (request == PICK_GAME_FOLDER) pickingGog = false;
            return;
        }
        Uri uri = data.getData();
        if (request == PICK_MOD) { installMod(uri); return; }
        if (request == PICK_ARCHIVE) { confirmArchive(uri); return; }
        if (request != PICK_GAME_FOLDER) return;
        try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
        catch (SecurityException ignored) { /* Direct paths do not depend on the grant. */ }
        File folder = treePath(uri);
        if (folder != null && folder.isDirectory() && folder.canRead()) selectFolder(folder, "");
        else if (pickingGog) { setStatus(I18n.t("Для GOG выберите папку в памяти устройства с EXE и всеми соседними BIN."), true); manualFolder(); }
        else confirmFolderCopy(uri);
    }
    private String displayName(Uri uri, String fallback) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && cursor.getString(0) != null) return cursor.getString(0);
        } catch (RuntimeException ignored) { /* Providers may refuse metadata. */ }
        return fallback;
    }
    private long documentSize(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) return cursor.getLong(0);
        } catch (RuntimeException ignored) { /* Unknown size. */ }
        return -1;
    }
    private static String safeName(String name) {
        String value = name == null ? "" : name.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").trim();
        return value.isEmpty() || value.equals(".") || value.equals("..") ? "game" : value;
    }
    private static File freshFolder(String name) {
        File base = new File(Environment.getExternalStorageDirectory(), "Stronghold"), folder = new File(base, name);
        for (int n = 2; folder.exists(); n++) folder = new File(base, name + " (" + n + ")");
        return folder;
    }
    private static String megabytes(long bytes) {
        return bytes < 1048576 ? String.format(Locale.ROOT, I18n.t("%.0f КБ"), Math.ceil(bytes / 1024.0))
                : String.format(Locale.ROOT, I18n.t("%.0f МБ"), bytes / 1048576.0);
    }
    private void confirmArchive(Uri archive) {
        String name = displayName(archive, "archive.zip");
        long size = documentSize(archive);
        String base = name.toLowerCase(Locale.ROOT).endsWith(".zip") ? name.substring(0, name.length() - 4) : name;
        File target = freshFolder(safeName(base));
        long free = new StatFs(Environment.getExternalStorageDirectory().getAbsolutePath()).getAvailableBytes();
        String message = I18n.t("Архив «") + name + "»" + (size > 0 ? " (" + megabytes(size) + ")" : "") + I18n.t(" будет распакован в\n") + target
                + I18n.t("\n\nСвободно: ") + megabytes(free) + I18n.t(". Распакованная игра занимает около 1 ГБ.");
        new AlertDialog.Builder(this).setTitle(I18n.t("Распаковать игру?")).setMessage(message)
                .setPositiveButton(I18n.t("Распаковать"), (dialog, which) -> transfer(I18n.t("Распаковка архива"), target, progress -> unzip(archive, target, size, progress)))
                .setNegativeButton(I18n.t("Отмена"), null).show();
    }
    private void confirmFolderCopy(Uri tree) {
        String name = displayName(DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree)), "game");
        File target = freshFolder(safeName(name));
        new AlertDialog.Builder(this).setTitle(I18n.t("Скопировать игру?"))
                .setMessage(I18n.t("К папке «") + name + I18n.t("» нет прямого доступа по пути. Скопировать её в\n") + target + "?")
                .setPositiveButton(I18n.t("Скопировать"), (dialog, which) -> transfer(I18n.t("Копирование папки"), target,
                        progress -> copyTree(tree, DocumentsContract.getTreeDocumentId(tree), target, progress)))
                .setNegativeButton(I18n.t("Отмена"), null).show();
    }
    private interface Progress { void update(long bytes, long total, int files); }
    private interface Work { void run(Progress progress) throws IOException; }
    /* Long copies keep their progress and cancellation in the files card. */
    private void transfer(String title, File target, Work work) {
        if (checking) return;
        checking = true;
        for (Button button : play) Theme.enable(button, false);
        transferCancelled = false;
        transferNotice = "";
        Progress progress = showProgress(fileProgress, title, () -> {
            transferCancelled = true; Process process = gogProcess; if (process != null) process.destroyForcibly();
        });
        setStatus(null, false);
        foldFiles();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            String error = null;
            try {
                if (!target.isDirectory() && !target.mkdirs()) throw new IOException(I18n.t("Не удалось создать папку ") + target);
                work.run(progress);
            } catch (InterruptedIOException cancelled) { error = I18n.t("Отменено. Частично скопированные файлы остались в ") + target;
            } catch (IOException | RuntimeException failure) { error = transferCancelled
                    ? I18n.t("Отменено. Частично скопированные файлы остались в ") + target : I18n.t("Ошибка: ") + failure.getMessage(); }
            final String problem = error;
            runOnUiThread(() -> {
                checking = false;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                fileProgress.removeAllViews();
                fileProgress.setVisibility(View.GONE);
                foldFiles();
                if (isFinishing() || isDestroyed()) return;
                if (problem != null) { setStatus(problem, true); detectGames(); return; }
                selectFolder(target, transferNotice);
            });
        }, "game-transfer").start();
    }
    private Progress showProgress(LinearLayout parent, String title, Runnable stop) {
        parent.removeAllViews(); parent.setVisibility(View.VISIBLE);
        LinearLayout content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL);
        note(content, title);
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(1000); bar.setIndeterminate(true);
        TextView label = Theme.text(this, I18n.t("Подготовка…"), 14, Theme.MUTED, false);
        content.addView(bar); content.addView(label);
        Button cancel = Theme.button(this, I18n.t("Отмена"), false);
        cancel.setOnClickListener(v -> { stop.run(); Theme.enable(cancel, false); });
        content.addView(cancel); parent.addView(content);
        long[] latest = {0, -1, 0};
        WorkProgress meter = new WorkProgress();
        label.post(new Runnable() {
            @Override public void run() {
                if (!checking || parent.getChildCount() == 0 || parent.getChildAt(0) != content || isFinishing() || isDestroyed()) return;
                long done, total, files;
                synchronized (latest) { done = latest[0]; total = latest[1]; files = latest[2]; }
                label.setText(meter.describe(I18n.language(), false, done, total, files, 0));
                bar.setIndeterminate(meter.percent < 0);
                if (meter.percent >= 0) bar.setProgress(meter.percent * 10);
                label.postDelayed(this, 250);
            }
        });
        return (bytes, total, files) -> { synchronized (latest) { latest[0] = bytes; latest[1] = total; latest[2] = files; } };
    }

    private static final class Counting extends FilterInputStream {
        long count;
        final java.util.function.LongConsumer changed;
        Counting(InputStream in) { this(in, null); }
        Counting(InputStream in, java.util.function.LongConsumer changed) { super(in); this.changed = changed; }
        @Override public int read() throws IOException { int b = super.read(); if (b >= 0) { count++; if (changed != null) changed.accept(count); } return b; }
        @Override public int read(byte[] data, int offset, int length) throws IOException {
            int n = super.read(data, offset, length); if (n > 0) { count += n; if (changed != null) changed.accept(count); } return n;
        }
    }
    private void copyStream(InputStream input, File file, java.util.function.LongConsumer progress) throws IOException {
        long done = 0;
        byte[] buffer = new byte[1 << 18];
        try (OutputStream output = new FileOutputStream(file)) {
            for (int n; (n = input.read(buffer)) > 0; ) {
                if (transferCancelled) throw new InterruptedIOException();
                output.write(buffer, 0, n);
                progress.accept(done += n);
            }
        }
    }
    private void unzip(Uri archive, File target, long size, Progress progress) throws IOException {
        String canonical = target.getCanonicalPath() + File.separator;
        int files = 0, unnamed = 0;
        try (InputStream raw = getContentResolver().openInputStream(archive)) {
            if (raw == null) throw new IOException(I18n.t("архив недоступен"));
            Counting counted = new Counting(new BufferedInputStream(raw, 1 << 16));
            try (ZipInputStream zip = new ZipInputStream(counted)) {
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                    String name = entry.getName().replace('\\', '/');
                    if (name.startsWith("__MACOSX/") || name.isEmpty()) continue;
                    File out = new File(target, name);
                    /* Entries cannot escape the destination ("zip slip"). */
                    if (!(out.getCanonicalPath() + (entry.isDirectory() ? File.separator : "")).startsWith(canonical))
                        throw new IOException(I18n.t("недопустимый путь в архиве: ") + name);
                    if (entry.isDirectory()) { if (!out.isDirectory() && !out.mkdirs()) throw new IOException(I18n.t("не удалось создать ") + out); continue; }
                    File parent = out.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException(I18n.t("не удалось создать ") + parent);
                    File intended = GameFiles.uniqueFile(out);
                    out = GameFiles.archiveStaging(intended, files);
                    final int completed = files;
                    copyStream(zip, out, bytes -> progress.update(counted.count, size, completed));
                    String recovered = GameFiles.archiveMapName(name, entry.getCrc());
                    if (!name.equals(recovered)) unnamed++;
                    File restored = name.equals(recovered) ? intended : new File(target, recovered);
                    restored = new File(restored.getParentFile(), safeName(restored.getName()));
                    if (!restored.getCanonicalPath().startsWith(canonical)) throw new IOException(I18n.t("недопустимое имя после восстановления: ") + recovered);
                    if (!out.equals(restored)) {
                        restored = GameFiles.uniqueFile(restored);
                        if (!out.renameTo(restored)) throw new IOException(I18n.t("не удалось восстановить имя карты: ") + restored);
                    }
                    progress.update(counted.count, size, ++files);
                }
            } catch (IllegalArgumentException names) {
                throw new IOException(I18n.t("имена файлов в архиве не в UTF-8; упакуйте игру заново стандартным ZIP"));
            }
        }
        if (files == 0) throw new IOException(I18n.t("архив пуст или не является ZIP"));
        if (unnamed > 0) transferNotice = I18n.t("\nВ архиве утрачены названия ") + unnamed + I18n.t(" карт: они сохранены как «Карта …». Все файлы сохранены отдельно.");
    }
    private void copyTree(Uri tree, String rootId, File target, Progress progress) throws IOException {
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        ArrayDeque<Object[]> folders = new ArrayDeque<>();
        folders.add(new Object[] {rootId, target});
        long bytes = 0; int files = 0;
        while (!folders.isEmpty()) {
            Object[] folder = folders.removeFirst();
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, (String)folder[0]);
            try (Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
                if (cursor == null) throw new IOException(I18n.t("поставщик файлов не отдал содержимое папки"));
                while (cursor.moveToNext()) {
                    if (transferCancelled) throw new InterruptedIOException();
                    File out = new File((File)folder[1], safeName(cursor.getString(1)));
                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                        if (!out.isDirectory() && !out.mkdirs()) throw new IOException(I18n.t("не удалось создать ") + out);
                        folders.add(new Object[] {cursor.getString(0), out});
                        continue;
                    }
                    try (InputStream input = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0)))) {
                        if (input == null) throw new IOException(I18n.t("не удалось открыть ") + out.getName());
                        final long previous = bytes; final int completed = files;
                        copyStream(new BufferedInputStream(input, 1 << 16), out, copied -> progress.update(previous + copied, -1, completed));
                    }
                    bytes += out.length();
                    progress.update(bytes, -1, ++files);
                }
            }
        }
    }
    static Class<? extends GameActivity> gameClass(String edition) {
        return "classic".equals(edition) ? ClassicGameActivity.class : GameFiles.original(edition) ? StrongholdGameActivity.class : ExtremeGameActivity.class;
    }
    private boolean running(String item) {
        String gamePid;
        try {
            gamePid = new String(java.nio.file.Files.readAllBytes(new File(getFilesDir(), "game-pid-" + item).toPath()),
                    java.nio.charset.StandardCharsets.US_ASCII);
        } catch (IOException error) { return false; }
        java.util.List<ActivityManager.RunningAppProcessInfo> processes = ((ActivityManager)getSystemService(ACTIVITY_SERVICE)).getRunningAppProcesses();
        if (processes != null) for (ActivityManager.RunningAppProcessInfo process : processes)
            if (process.processName.equals(getPackageName() + ":" + item) && Integer.toString(process.pid).equals(gamePid)) return true;
        return false;
    }
    private void attempt(String item, String directory, boolean skipMovies) {
        if (checking) return;
        preferences.edit().putString(pathKey(item), directory).putBoolean("skipMovies", skipMovies).putString("edition", item).commit();
        if (requestStorage()) { showLaunchError(item, I18n.t("Для запуска нужен доступ к файлам игры.")); return; }
        checking = true;
        setStatus(gameNotes[editionIndex(item)], I18n.t("Проверка файлов…"), false);
        new Thread(() -> {
            GameFiles.Check check = GameFiles.check(new File(directory), item, true);
            String images = null, preparationError = null;
            if (check.error == null) try {
                images = GameImages.prepare(this, new File(directory), item).getAbsolutePath();
                NativeLibrary.prepare(this, GameFiles.library(item));
            } catch (IOException | UnsatisfiedLinkError failure) { preparationError = I18n.t("Не удалось подготовить игру: ") + failure.getMessage(); }
            final String preparedImages = images, failedPreparation = preparationError;
            runOnUiThread(() -> {
                checking = false;
                if (isFinishing() || isDestroyed()) return;
                if (check.error != null) { showLaunchError(item, check.error); return; }
                if (failedPreparation != null) { showLaunchError(item, failedPreparation); return; }
                if (check.warning != null) {
                    checking = true;
                    new AlertDialog.Builder(this).setTitle(I18n.t("Предупреждение о совместимости"))
                            .setMessage(check.warning)
                            .setPositiveButton(I18n.t("Запустить всё равно"), (dialog, which) -> launch(item, directory, preparedImages, skipMovies))
                            .setNegativeButton(I18n.t("Отмена"), null)
                            .setOnDismissListener(dialog -> { checking = false; if (!isFinishing() && !showingLauncher) showLauncher(null); })
                            .show();
                } else launch(item, directory, preparedImages, skipMovies);
            });
        }, "game-preflight").start();
    }
    private void launch(String item, String directory, String images, boolean skipMovies) {
        try {
            if (!running(item)) {
                File marker = GameFiles.pending(getFilesDir(), item);
                if (marker.exists() && !marker.delete()) throw new IOException(I18n.t("Не удалось обновить состояние запуска"));
                if (!marker.createNewFile()) throw new IOException(I18n.t("Не удалось сохранить состояние запуска"));
            }
            showingLauncher = false; selectingFiles = false; ++scanVersion;
            startActivity(new Intent(this, gameClass(item)).putExtra("game", directory).putExtra("images", images).putExtra("skipMovies", skipMovies)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_NO_ANIMATION),
                    android.app.ActivityOptions.makeCustomAnimation(this, 0, 0).toBundle());
            overridePendingTransition(0, 0);
            for (ActivityManager.AppTask task : ((ActivityManager)getSystemService(ACTIVITY_SERVICE)).getAppTasks())
                if (task.getTaskInfo().baseActivity == null) task.finishAndRemoveTask();
            finishAndRemoveTask();
        } catch (IOException | RuntimeException failure) { showLaunchError(item, I18n.t("Не удалось запустить игру: ") + failure.getMessage()); }
    }
    private void handleIntent(Intent intent) {
        I18n.apply(this);
        selectingFiles = false; ++scanVersion;
        String[] successful = GameFiles.lastSuccess(getFilesDir());
        String requested = intent.getStringExtra("edition");
        String edition = requested != null ? requested : successful != null ? successful[0] : preferences.getString("edition", "extreme");
        edition = "classic".equals(edition) || GameFiles.original(edition) ? edition : "extreme";
        String error = intent.getStringExtra("error");
        if (GameFiles.pending(getFilesDir(), edition).exists() && !running(edition)) {
            if (error == null) error = I18n.t("Игра завершилась с ошибкой. Проверьте папку и повторите запуск. Журнал: /sdcard/Stronghold/")
                    + GameFiles.userdata(edition) + "/stronghold.log";
            GameFiles.pending(getFilesDir(), edition).delete();
        }
        if (error != null || intent.getBooleanExtra("showLauncher", false)
                || (!intent.getBooleanExtra("launchGame", false) && successful == null)) {
            showLauncher(error); return;
        }
        /* The successful-start record is shared across the isolated game processes. */
        showingLauncher = false; ++scanVersion;
        TextView starting = Theme.text(this, I18n.t("Запуск ") + GameFiles.title(edition) + "…", 16, Theme.MUTED, false);
        starting.setGravity(Gravity.CENTER); setContentView(starting);
        String directory = requested == null && successful != null ? successful[1] : gamePath(edition);
        attempt(edition, directory, preferences.getBoolean("skipMovies", false));
    }
    @Override protected void onNewIntent(Intent intent) { super.onNewIntent(intent); setIntent(intent); handleIntent(intent); }
    @Override protected void onResume() {
        super.onResume();
        I18n.apply(this);
        updateStorageAccess();
        if (showingLauncher) detectGames();
    }
}
