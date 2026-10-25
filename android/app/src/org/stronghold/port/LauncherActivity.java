package org.stronghold.port;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
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
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** The launcher never loads JNI; each game process loads exactly one edition. */
public final class LauncherActivity extends Activity {
    private static final int PICK_GAME_FOLDER = 11, PICK_ARCHIVE = 13;
    /* Tile order: Crusader, Crusader Extreme, the original Stronghold HD. */
    private static final String[] EDITIONS = GameFiles.EDITIONS;
    private static final String EXTERNAL_STORAGE = "com.android.externalstorage.documents";
    private SharedPreferences preferences;
    private LinearLayout setupActions;
    private Button changeFiles;
    private TextView filesHint;
    private boolean showingLauncher, selectingFiles;
    private Switch skip;
    private TextView status;
    private final TextView[] gameStatus = new TextView[3];
    private Button access;
    private final Button[] play = new Button[3];
    private int scanVersion;
    private boolean checking;
    private volatile boolean transferCancelled;
    private String transferNotice;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Theme.edgeToEdge(this);
        preferences = getSharedPreferences("LauncherActivity", MODE_PRIVATE);
        handleIntent(getIntent());
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
        titles.addView(Theme.text(this, "Stronghold HD, Crusader и Crusader Extreme · Android ARM64", 13, Theme.MUTED, false));
        header.addView(titles, new LinearLayout.LayoutParams(0, -2, 1));
        access = Theme.button(this, "", false); access.setOnClickListener(v -> openStorageSettings());
        if (wide) header.addView(access, new LinearLayout.LayoutParams(-2, -2));
        page.addView(header);
        if (!wide) { gap(page, 8); page.addView(access, new LinearLayout.LayoutParams(-1, -2)); }
        gap(page, 14);

        status = Theme.text(this, error == null ? "Выберите игру" : error, 14, error == null ? Theme.MUTED : Theme.BAD, false);
        status.setPadding(dp(4), 0, dp(4), dp(10)); page.addView(status);

        LinearLayout source = Theme.card(this);
        heading(source, "Файлы игр");
        filesHint = note(source, "Проверка установленных игр…");
        setupActions = Theme.row(this);
        Button choose = Theme.button(this, "Выбрать папку…", false);
        choose.setOnClickListener(v -> { if (!requestStorage()) pickFolder(); });
        Button archive = Theme.button(this, "Распаковать ZIP…", false);
        archive.setOnClickListener(v -> { if (!requestStorage()) pickArchive(); });
        setupActions.addView(choose, Theme.weight(this, 1, 0)); setupActions.addView(archive, Theme.weight(this, 1, 8));
        source.addView(setupActions);
        changeFiles = Theme.button(this, "Изменить файлы игр…", false);
        changeFiles.setVisibility(View.GONE); changeFiles.setOnClickListener(v -> fileActions());
        source.addView(changeFiles, new LinearLayout.LayoutParams(-1, -2));
        page.addView(source);

        LinearLayout columns = new LinearLayout(this); columns.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        LinearLayout games = new LinearLayout(this), files = new LinearLayout(this);
        games.setOrientation(LinearLayout.VERTICAL); files.setOrientation(LinearLayout.VERTICAL);
        columns.addView(games, wide ? Theme.weight(this, 1, 0) : new LinearLayout.LayoutParams(-1, -2));
        columns.addView(files, wide ? Theme.weight(this, 1.15f, 14) : new LinearLayout.LayoutParams(-1, -2));
        page.addView(columns);
        for (int i = 0; i < EDITIONS.length; i++) games.addView(gameTile(i));

        LinearLayout options = Theme.card(this);
        heading(options, "Запуск");
        skip = new Switch(this); skip.setText("Пропускать ролики, рекламу и предупреждение об онлайне");
        skip.setTextColor(Theme.TEXT); skip.setChecked(preferences.getBoolean("skipMovies", false)); skip.setPadding(0, dp(6), 0, dp(6));
        skip.setOnCheckedChangeListener((button, checked) -> preferences.edit().putBoolean("skipMovies", checked).apply());
        options.addView(skip);
        note(options, "Параметры запуска применяются при новом запуске процесса игры. Перед перезапуском сохраните матч.");
        files.addView(options);

        frame.setOnApplyWindowInsetsListener((view, insets) -> Theme.padInsets(page, insets, dp(20), dp(14), dp(20), dp(20)));
        setContentView(frame);
        updateStorageAccess();
        detectGames();
    }
    private int resource(String name, String type) { return getResources().getIdentifier(name, type, getPackageName()); }
    private View gameTile(int index) {
        final String item = EDITIONS[index];
        LinearLayout card = Theme.card(this);
        LinearLayout top = Theme.row(this);
        ImageView icon = new ImageView(this); icon.setImageResource(resource(index == 0 ? "ic_classic" : index == 1 ? "ic_extreme" : "ic_stronghold", "mipmap"));
        top.addView(icon, new LinearLayout.LayoutParams(dp(56), dp(56)));
        LinearLayout titles = new LinearLayout(this); titles.setOrientation(LinearLayout.VERTICAL); titles.setPadding(dp(12), 0, 0, 0);
        titles.addView(Theme.text(this, index == 0 ? "Stronghold Crusader" : index == 1 ? "Crusader Extreme" : "Stronghold HD", 19, Theme.TEXT, true));
        gameStatus[index] = Theme.text(this, "Проверка файлов…", 13, Theme.MUTED, false);
        titles.addView(gameStatus[index]);
        top.addView(titles, new LinearLayout.LayoutParams(0, -2, 1));
        card.addView(top);
        gap(card, 10);
        LinearLayout actions = Theme.row(this);
        Button button = Theme.button(this, "Играть", true); play[index] = button; Theme.enable(button, false);
        button.setOnClickListener(v -> attempt(item, gamePath(item), skip.isChecked()));
        Button settings = Theme.button(this, "Настройки", false);
        settings.setOnClickListener(v -> startActivity(new Intent(this, settingsClass(item))));
        actions.addView(button, Theme.weight(this, 1.3f, 0)); actions.addView(settings, Theme.weight(this, 1, 8));
        card.addView(actions);
        return card;
    }
    private void fileActions() {
        new AlertDialog.Builder(this).setTitle("Файлы игр")
                .setItems(new String[] {"Выбрать папку…", "Распаковать ZIP…", "Указать папку вручную…", "Проверить снова"}, (dialog, which) -> {
                    if (requestStorage()) return;
                    if (which == 0) pickFolder();
                    else if (which == 1) pickArchive();
                    else if (which == 2) manualFolder();
                    else detectGames();
                }).show();
    }
    private void manualFolder() {
        EditText folder = Theme.field(this, filesFolder().getAbsolutePath(), "Папка игры или общая папка с играми");
        new AlertDialog.Builder(this).setTitle("Папка с файлами игр").setView(folder)
                .setPositiveButton("Проверить", (dialog, which) -> selectFolder(new File(folder.getText().toString().trim()), ""))
                .setNegativeButton("Отмена", null).show();
    }
    private void setStatus(String text, boolean problem) {
        if (status == null) return;
        status.setText(text); status.setTextColor(problem ? Theme.BAD : Theme.MUTED);
    }
    private void detectGames() {
        if (selectingFiles) return; // Returning from the picker must not cancel its pending validation.
        scanGames(new File(Environment.getExternalStorageDirectory(), "Stronghold"), false, null);
    }
    private void selectFolder(File folder, String notice) {
        preferences.edit().putString("filesFolder", folder.getAbsolutePath()).apply();
        scanGames(folder, true, "Файлы игр в " + folder + notice);
    }
    private void scanGames(File source, boolean replaceMatching, String notice) {
        if (!showingLauncher) return;
        final int scan = ++scanVersion;
        selectingFiles = replaceMatching;
        final String[] saved = savedPaths();
        new Thread(() -> {
            GameFiles.Scan result = GameFiles.scan(saved, source, replaceMatching, true);
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
                    gameStatus[i].setText(valid ? warning == null ? "Готов к запуску" : warning : problem);
                    gameStatus[i].setTextColor(valid ? warning == null ? Theme.GOOD : Theme.ACCENT : Theme.BAD);
                    Theme.enable(play[i], valid);
                }
                settings.apply();
                boolean complete = result.complete();
                setupActions.setVisibility(complete ? View.GONE : View.VISIBLE);
                changeFiles.setVisibility(complete ? View.VISIBLE : View.GONE);
                filesHint.setText(complete ? "Все три игры найдены и проверены."
                        : "Добавьте файлы: " + missing + ". Папка или ZIP распознаются автоматически; можно выбрать общую папку с играми.");
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
        access.setText("Разрешить доступ к файлам");
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
        catch (ActivityNotFoundException missing) { setStatus("Системный выбор папок недоступен на этом устройстве.", true); manualFolder(); }
    }
    private void pickArchive() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/x-zip", "application/octet-stream"});
        try { startActivityForResult(intent, PICK_ARCHIVE); }
        catch (ActivityNotFoundException missing) { setStatus("Системный выбор файлов недоступен на этом устройстве.", true); }
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
        if (result != RESULT_OK || data == null || data.getData() == null || !showingLauncher) return;
        Uri uri = data.getData();
        if (request == PICK_ARCHIVE) { confirmArchive(uri); return; }
        if (request != PICK_GAME_FOLDER) return;
        try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
        catch (SecurityException ignored) { /* Direct paths do not depend on the grant. */ }
        File folder = treePath(uri);
        if (folder != null && folder.isDirectory() && folder.canRead()) selectFolder(folder, "");
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
        return bytes < 1048576 ? String.format(Locale.ROOT, "%.0f КБ", Math.ceil(bytes / 1024.0))
                : String.format(Locale.ROOT, "%.0f МБ", bytes / 1048576.0);
    }
    private void confirmArchive(Uri archive) {
        String name = displayName(archive, "archive.zip");
        long size = documentSize(archive);
        String base = name.toLowerCase(Locale.ROOT).endsWith(".zip") ? name.substring(0, name.length() - 4) : name;
        File target = freshFolder(safeName(base));
        long free = new StatFs(Environment.getExternalStorageDirectory().getAbsolutePath()).getAvailableBytes();
        String message = "Архив «" + name + "»" + (size > 0 ? " (" + megabytes(size) + ")" : "") + " будет распакован в\n" + target
                + "\n\nСвободно: " + megabytes(free) + ". Распакованная игра занимает около 1 ГБ.";
        new AlertDialog.Builder(this).setTitle("Распаковать игру?").setMessage(message)
                .setPositiveButton("Распаковать", (dialog, which) -> transfer("Распаковка архива", target, progress -> unzip(archive, target, size, progress)))
                .setNegativeButton("Отмена", null).show();
    }
    private void confirmFolderCopy(Uri tree) {
        String name = displayName(DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree)), "game");
        File target = freshFolder(safeName(name));
        new AlertDialog.Builder(this).setTitle("Скопировать игру?")
                .setMessage("К папке «" + name + "» нет прямого доступа по пути. Скопировать её в\n" + target + "?")
                .setPositiveButton("Скопировать", (dialog, which) -> transfer("Копирование папки", target,
                        progress -> copyTree(tree, DocumentsContract.getTreeDocumentId(tree), target, progress)))
                .setNegativeButton("Отмена", null).show();
    }
    private interface Progress { void update(long bytes, long total, int files); }
    private interface Work { void run(Progress progress) throws IOException; }
    /* Long copies run off the UI thread with a cancellable progress dialog. */
    private void transfer(String title, File target, Work work) {
        transferCancelled = false;
        transferNotice = "";
        LinearLayout content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), dp(12), dp(24), dp(4));
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(1000); bar.setIndeterminate(true);
        TextView label = Theme.text(this, "Подготовка…", 14, Theme.MUTED, false); label.setPadding(0, dp(8), 0, 0);
        content.addView(bar); content.addView(label);
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(title).setView(content).setCancelable(false)
                .setNegativeButton("Отмена", (d, which) -> transferCancelled = true).show();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        long[] shown = {0};
        Progress progress = (bytes, total, files) -> {
            long now = System.nanoTime();
            if (now - shown[0] < 120_000_000L) return;
            shown[0] = now;
            runOnUiThread(() -> {
                if (total > 0) { bar.setIndeterminate(false); bar.setProgress((int)Math.min(1000, bytes * 1000 / total)); }
                label.setText(files + " файлов · " + megabytes(bytes) + (total > 0 ? " из " + megabytes(total) : ""));
            });
        };
        new Thread(() -> {
            String error = null;
            try {
                if (!target.isDirectory() && !target.mkdirs()) throw new IOException("Не удалось создать папку " + target);
                work.run(progress);
            } catch (InterruptedIOException cancelled) { error = "Отменено. Частично скопированные файлы остались в " + target;
            } catch (IOException | RuntimeException failure) { error = "Ошибка: " + failure.getMessage(); }
            final String problem = error;
            runOnUiThread(() -> {
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                if (dialog.isShowing()) dialog.dismiss();
                if (isFinishing() || isDestroyed()) return;
                if (problem != null) { setStatus(problem, true); return; }
                selectFolder(target, transferNotice);
            });
        }, "game-transfer").start();
    }
    private static final class Counting extends FilterInputStream {
        long count;
        Counting(InputStream in) { super(in); }
        @Override public int read() throws IOException { int b = super.read(); if (b >= 0) count++; return b; }
        @Override public int read(byte[] data, int offset, int length) throws IOException {
            int n = super.read(data, offset, length); if (n > 0) count += n; return n;
        }
    }
    private void copyStream(InputStream input, File file) throws IOException {
        byte[] buffer = new byte[1 << 18];
        try (OutputStream output = new FileOutputStream(file)) {
            for (int n; (n = input.read(buffer)) > 0; ) {
                if (transferCancelled) throw new InterruptedIOException();
                output.write(buffer, 0, n);
            }
        }
    }
    private void unzip(Uri archive, File target, long size, Progress progress) throws IOException {
        String canonical = target.getCanonicalPath() + File.separator;
        int files = 0, unnamed = 0;
        try (InputStream raw = getContentResolver().openInputStream(archive)) {
            if (raw == null) throw new IOException("архив недоступен");
            Counting counted = new Counting(new BufferedInputStream(raw, 1 << 16));
            try (ZipInputStream zip = new ZipInputStream(counted)) {
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                    String name = entry.getName().replace('\\', '/');
                    if (name.startsWith("__MACOSX/") || name.isEmpty()) continue;
                    File out = new File(target, name);
                    /* Entries cannot escape the destination ("zip slip"). */
                    if (!(out.getCanonicalPath() + (entry.isDirectory() ? File.separator : "")).startsWith(canonical))
                        throw new IOException("недопустимый путь в архиве: " + name);
                    if (entry.isDirectory()) { if (!out.isDirectory() && !out.mkdirs()) throw new IOException("не удалось создать " + out); continue; }
                    File parent = out.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("не удалось создать " + parent);
                    File intended = GameFiles.uniqueFile(out);
                    out = GameFiles.archiveStaging(intended, files);
                    copyStream(zip, out);
                    String recovered = GameFiles.archiveMapName(name, entry.getCrc());
                    if (!name.equals(recovered)) unnamed++;
                    File restored = name.equals(recovered) ? intended : new File(target, recovered);
                    restored = new File(restored.getParentFile(), safeName(restored.getName()));
                    if (!restored.getCanonicalPath().startsWith(canonical)) throw new IOException("недопустимое имя после восстановления: " + recovered);
                    if (!out.equals(restored)) {
                        restored = GameFiles.uniqueFile(restored);
                        if (!out.renameTo(restored)) throw new IOException("не удалось восстановить имя карты: " + restored);
                    }
                    progress.update(counted.count, size, ++files);
                }
            } catch (IllegalArgumentException names) {
                throw new IOException("имена файлов в архиве не в UTF-8; упакуйте игру заново стандартным ZIP");
            }
        }
        if (files == 0) throw new IOException("архив пуст или не является ZIP");
        if (unnamed > 0) transferNotice = "\nВ архиве утрачены названия " + unnamed + " карт: они сохранены как «Карта …». Все файлы сохранены отдельно.";
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
                if (cursor == null) throw new IOException("поставщик файлов не отдал содержимое папки");
                while (cursor.moveToNext()) {
                    if (transferCancelled) throw new InterruptedIOException();
                    File out = new File((File)folder[1], safeName(cursor.getString(1)));
                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                        if (!out.isDirectory() && !out.mkdirs()) throw new IOException("не удалось создать " + out);
                        folders.add(new Object[] {cursor.getString(0), out});
                        continue;
                    }
                    try (InputStream input = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0)))) {
                        if (input == null) throw new IOException("не удалось открыть " + out.getName());
                        copyStream(new BufferedInputStream(input, 1 << 16), out);
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
        if (requestStorage()) { showLauncher("Для запуска нужен доступ к файлам игры."); return; }
        checking = true;
        setStatus("Проверка файлов…", false);
        new Thread(() -> {
            GameFiles.Check check = GameFiles.check(new File(directory), item, true);
            runOnUiThread(() -> {
                checking = false;
                if (isFinishing() || isDestroyed()) return;
                if (check.error != null) { showLauncher(check.error); return; }
                if (check.warning != null) {
                    checking = true;
                    new AlertDialog.Builder(this).setTitle("Предупреждение о совместимости")
                            .setMessage(check.warning)
                            .setPositiveButton("Запустить всё равно", (dialog, which) -> launch(item, directory, skipMovies))
                            .setNegativeButton("Отмена", null)
                            .setOnDismissListener(dialog -> { checking = false; if (!isFinishing() && !showingLauncher) showLauncher(null); })
                            .show();
                } else launch(item, directory, skipMovies);
            });
        }, "game-preflight").start();
    }
    private void launch(String item, String directory, boolean skipMovies) {
        try {
            if (!running(item)) {
                File marker = GameFiles.pending(getFilesDir(), item);
                if (marker.exists() && !marker.delete()) throw new IOException("Не удалось обновить состояние запуска");
                if (!marker.createNewFile()) throw new IOException("Не удалось сохранить состояние запуска");
            }
            showingLauncher = false; selectingFiles = false; ++scanVersion;
            startActivity(new Intent(this, gameClass(item)).putExtra("game", directory).putExtra("skipMovies", skipMovies)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_NO_ANIMATION),
                    android.app.ActivityOptions.makeCustomAnimation(this, 0, 0).toBundle());
            overridePendingTransition(0, 0);
            for (ActivityManager.AppTask task : ((ActivityManager)getSystemService(ACTIVITY_SERVICE)).getAppTasks())
                if (task.getTaskInfo().baseActivity == null) task.finishAndRemoveTask();
            finishAndRemoveTask();
        } catch (IOException | RuntimeException failure) { showLauncher("Не удалось запустить игру: " + failure.getMessage()); }
    }
    private void handleIntent(Intent intent) {
        selectingFiles = false; ++scanVersion;
        String[] successful = GameFiles.lastSuccess(getFilesDir());
        String requested = intent.getStringExtra("edition");
        String edition = requested != null ? requested : successful != null ? successful[0] : preferences.getString("edition", "extreme");
        edition = "classic".equals(edition) || GameFiles.original(edition) ? edition : "extreme";
        String error = intent.getStringExtra("error");
        if (GameFiles.pending(getFilesDir(), edition).exists() && !running(edition)) {
            if (error == null) error = "Игра завершилась с ошибкой. Проверьте папку и повторите запуск. Журнал: /sdcard/Stronghold/"
                    + GameFiles.userdata(edition) + "/stronghold.log";
            GameFiles.pending(getFilesDir(), edition).delete();
        }
        if (error != null || intent.getBooleanExtra("showLauncher", false)
                || (!intent.getBooleanExtra("launchGame", false) && successful == null)) {
            showLauncher(error); return;
        }
        /* The successful-start record is shared across the isolated game processes. */
        showingLauncher = false; ++scanVersion;
        TextView starting = Theme.text(this, "Запуск " + GameFiles.title(edition) + "…", 16, Theme.MUTED, false);
        starting.setGravity(Gravity.CENTER); setContentView(starting);
        String directory = requested == null && successful != null ? successful[1] : gamePath(edition);
        attempt(edition, directory, preferences.getBoolean("skipMovies", false));
    }
    @Override protected void onNewIntent(Intent intent) { super.onNewIntent(intent); setIntent(intent); handleIntent(intent); }
    @Override protected void onResume() {
        super.onResume();
        updateStorageAccess();
        if (showingLauncher) detectGames();
    }
}
