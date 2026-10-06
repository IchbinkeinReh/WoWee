package com.wowee.client;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.format.Formatter;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;

/**
 * Where the game data is, asked before the client starts.
 *
 * The data is the player's own extraction, several gigabytes copied over from
 * a PC. It used to have to go into the app's own external files directory,
 * which Android deletes with the app - an uninstall to replace a build took the
 * data with it. Now it lives in a folder the player picks, /sdcard/wowee unless
 * they pick another, and survives the app.
 *
 * The client opens its files by path from native code, which the storage
 * access framework cannot serve, so a folder outside the app's own needs
 * all-files access. The folder picker is only used to choose a path.
 *
 * The data can also arrive as one zip, picked here or left at a well-known
 * place (wellKnownZip), which ZipImport unpacks into the folder with a progress
 * bar. One large file is easier to get onto a phone than 200,000 small ones.
 *
 * This screen is skipped when access is granted and the folder holds data. The
 * launcher shortcut "Data folder" opens it regardless, to choose again.
 */
public class DataFolderActivity extends Activity implements ZipImport.Listener {

    static final String EXTRA_CHOOSE = "com.wowee.client.CHOOSE_FOLDER";

    private static final String PREFS = "wowee";
    private static final String KEY_DATA_PATH = "dataPath";
    private static final int REQUEST_TREE = 1;
    private static final int REQUEST_ZIP = 2;

    private boolean choosing;
    private TextView status;
    private Button primary;
    private LinearLayout buttons;
    private Button unpackZip;
    private Button unpackFound;

    // Shown instead of the buttons while a zip is being unpacked.
    private LinearLayout progressPanel;
    private ProgressBar progress;
    private TextView progressText;
    private TextView progressEntry;
    private Button cancel;
    private ZipImport job;

    /** The folder the player chose, or /sdcard/wowee. */
    static File chosenFolder(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, MODE_PRIVATE);
        String path = prefs.getString(KEY_DATA_PATH, null);
        if (path == null || path.isEmpty()) {
            return new File(Environment.getExternalStorageDirectory(), "wowee");
        }
        return new File(path);
    }

    /**
     * The directory the client reads as its Data: the chosen folder itself, or
     * a Data folder inside it when the player picked the folder around it.
     */
    static File dataDir(Context context) {
        File folder = chosenFolder(context);
        File inner = new File(folder, "Data");
        if (!hasGameData(folder) && hasGameData(inner)) return inner;
        return folder;
    }

    /**
     * A manifest at the top, or under expansions/<id>/ - which is where
     * extract_assets.sh and the asset builder put one.
     */
    static boolean hasGameData(File dir) {
        if (new File(dir, "manifest.json").isFile()) return true;
        File[] expansions = new File(dir, "expansions").listFiles();
        if (expansions == null) return false;
        for (File e : expansions) {
            if (new File(e, "manifest.json").isFile()) return true;
        }
        return false;
    }

    private static boolean haveAccess() {
        return Environment.isExternalStorageManager();
    }

    /**
     * A zip of the data left where the app looks without being asked:
     * wowee-data.zip in the data folder itself or in Download.
     */
    static File wellKnownZip(Context context) {
        File[] candidates = {
            new File(chosenFolder(context), ZipImport.WELL_KNOWN_NAME),
            new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                     ZipImport.WELL_KNOWN_NAME),
        };
        for (File f : candidates) {
            if (f.isFile()) return f;
        }
        return null;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        choosing = getIntent().getBooleanExtra(EXTRA_CHOOSE, false);
        buildView();
        if (haveAccess()) ZipImport.cleanStale(chosenFolder(this));
    }

    @Override
    protected void onResume() {
        super.onResume();
        // An import outlives a recreated screen; pick it back up, and let it
        // decide what is shown until it is over.
        job = ZipImport.current();
        if (job != null) {
            job.setListener(this);
            return;
        }
        // Back from the settings screen, the picker, or the player copying data
        // in: the state may have changed under this screen either way.
        if (!choosing && haveAccess() && hasGameData(dataDir(this))) {
            startGame();
            return;
        }
        refresh();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (job != null) job.setListener(null);
    }

    private void buildView() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(Color.BLACK);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.data_folder_title);
        title.setTextColor(Color.WHITE);
        title.setTextSize(22);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.LTGRAY);
        status.setTextSize(15);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, pad / 2, 0, pad);
        layout.addView(status);

        buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.VERTICAL);
        layout.addView(buttons, buttonParams());

        primary = new Button(this);
        buttons.addView(primary, buttonParams());

        unpackFound = new Button(this);
        buttons.addView(unpackFound, buttonParams());

        unpackZip = new Button(this);
        unpackZip.setText(R.string.data_folder_zip);
        unpackZip.setOnClickListener(v -> chooseZip());
        buttons.addView(unpackZip, buttonParams());

        Button choose = new Button(this);
        choose.setText(R.string.data_folder_choose);
        choose.setOnClickListener(v -> chooseFolder());
        buttons.addView(choose, buttonParams());

        Button reset = new Button(this);
        reset.setText(R.string.data_folder_default);
        reset.setOnClickListener(v -> {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(KEY_DATA_PATH).apply();
            refresh();
        });
        buttons.addView(reset, buttonParams());

        progressPanel = new LinearLayout(this);
        progressPanel.setOrientation(LinearLayout.VERTICAL);
        progressPanel.setVisibility(View.GONE);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progressPanel.addView(progress, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        progressText = new TextView(this);
        progressText.setTextColor(Color.WHITE);
        progressText.setTextSize(15);
        progressText.setGravity(Gravity.CENTER);
        progressPanel.addView(progressText);

        progressEntry = new TextView(this);
        progressEntry.setTextColor(Color.GRAY);
        progressEntry.setTextSize(12);
        progressEntry.setGravity(Gravity.CENTER);
        progressEntry.setSingleLine(true);
        progressEntry.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        progressEntry.setPadding(0, 0, 0, pad / 2);
        progressPanel.addView(progressEntry);

        cancel = new Button(this);
        cancel.setText(R.string.zip_cancel);
        cancel.setOnClickListener(v -> {
            if (job != null) job.cancel();
        });
        progressPanel.addView(cancel, buttonParams());

        LinearLayout.LayoutParams panelParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        layout.addView(progressPanel, panelParams);

        setContentView(layout);
    }

    private LinearLayout.LayoutParams buttonParams() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.gravity = Gravity.CENTER_HORIZONTAL;
        return p;
    }

    private void refresh() {
        File folder = chosenFolder(this);
        showProgress(false);
        if (!haveAccess()) {
            status.setText(getString(R.string.data_folder_need_access, folder.getAbsolutePath()));
            primary.setText(R.string.data_folder_grant);
            primary.setOnClickListener(v -> requestAccess());
            // Unpacking writes into the folder, which needs the same access.
            unpackZip.setVisibility(View.GONE);
            unpackFound.setVisibility(View.GONE);
            return;
        }
        boolean found = hasGameData(dataDir(this));
        status.setText(getString(found ? R.string.data_folder_found : R.string.data_folder_missing,
                folder.getAbsolutePath()));
        primary.setText(R.string.data_folder_start);
        primary.setOnClickListener(v -> startGame());

        unpackZip.setVisibility(View.VISIBLE);
        File zip = wellKnownZip(this);
        if (zip != null) {
            unpackFound.setText(getString(R.string.data_folder_zip_found, zip.getAbsolutePath()));
            unpackFound.setOnClickListener(v -> confirmImport(
                    () -> ZipImport.start(this, zip, chosenFolder(this))));
            unpackFound.setVisibility(View.VISIBLE);
        } else {
            unpackFound.setVisibility(View.GONE);
        }
    }

    private void chooseZip() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // Providers disagree on what a zip is called.
        intent.setType("application/zip");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
            "application/zip", "application/x-zip-compressed", "application/x-zip",
        });
        startActivityForResult(intent, REQUEST_ZIP);
    }

    /** Asks before unpacking over data that is already there, then starts. */
    private void confirmImport(Runnable start) {
        Runnable begin = () -> {
            try {
                start.run();
            } catch (IllegalStateException e) {
                // One already running; onResume will have attached to it.
            }
            job = ZipImport.current();
            if (job != null) job.setListener(this);
        };
        if (!hasGameData(dataDir(this))) {
            begin.run();
            return;
        }
        new AlertDialog.Builder(this)
                .setMessage(getString(R.string.zip_replace, chosenFolder(this).getAbsolutePath()))
                .setPositiveButton(R.string.zip_replace_ok, (d, w) -> begin.run())
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void importPicked(Uri uri) {
        String name = uri.getLastPathSegment();
        long size = -1;
        try (Cursor c = getContentResolver().query(uri,
                new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE },
                null, null, null)) {
            if (c != null && c.moveToFirst()) {
                if (!c.isNull(0)) name = c.getString(0);
                if (!c.isNull(1)) size = c.getLong(1);
            }
        } catch (RuntimeException e) {
            // Name and size are for display and the space check; go without.
        }
        // A zip on the device's own storage can be opened by path, which reads
        // its directory for exact sizes. Anything else is read as a stream.
        String path = null;
        try {
            path = pathOfDocument(uri.getAuthority(), DocumentsContract.getDocumentId(uri));
        } catch (IllegalArgumentException e) {
            // Not a document URI; read it as a stream.
        }
        File file = path != null ? new File(path) : null;
        File target = chosenFolder(this);
        if (file != null && file.canRead()) {
            confirmImport(() -> ZipImport.start(this, file, target));
        } else {
            String shown = name;
            long known = size;
            confirmImport(() -> ZipImport.start(this, uri, shown, known, target));
        }
    }

    @Override
    public void onImportChanged(ZipImport j) {
        switch (j.state()) {
            case RUNNING:
                showProgress(true);
                status.setText(getString(R.string.zip_importing, j.sourceName,
                        j.target.getAbsolutePath()));
                long total = j.total();
                long done = j.done();
                if (j.cancelRequested()) {
                    progress.setIndeterminate(true);
                    progressText.setText(R.string.zip_cancelling);
                    progressEntry.setText("");
                    cancel.setEnabled(false);
                } else if (j.finishing()) {
                    progress.setIndeterminate(true);
                    progressText.setText(R.string.zip_finishing);
                    progressEntry.setText("");
                    cancel.setEnabled(false);
                } else if (total > 0) {
                    int permille = (int) Math.min(1000, done * 1000 / total);
                    progress.setIndeterminate(false);
                    progress.setProgress(permille);
                    progressText.setText(getString(R.string.zip_progress, permille / 10,
                            Formatter.formatShortFileSize(this, done),
                            Formatter.formatShortFileSize(this, total)));
                    progressEntry.setText(j.entry());
                    cancel.setEnabled(true);
                } else {
                    progress.setIndeterminate(true);
                    progressText.setText(getString(R.string.zip_progress_unknown,
                            Formatter.formatShortFileSize(this, done)));
                    progressEntry.setText(j.entry());
                    cancel.setEnabled(true);
                }
                break;
            case DONE:
                ZipImport.clear(j);
                job = null;
                showProgress(false);
                // On into the game the way a folder copied in by hand does.
                if (hasGameData(dataDir(this))) {
                    startGame();
                } else {
                    refresh();
                }
                break;
            case FAILED:
                ZipImport.clear(j);
                job = null;
                refresh();
                status.setText(getString(R.string.zip_failed, j.sourceName, j.error()));
                break;
            case CANCELLED:
                ZipImport.clear(j);
                job = null;
                refresh();
                status.setText(R.string.zip_cancelled);
                break;
        }
    }

    /**
     * Swaps the buttons for the progress bar. While it shows, the screen stays
     * on and the orientation is held, so nothing recreates the screen or sends
     * the device to sleep under a long unpack.
     */
    private void showProgress(boolean on) {
        if (on == (progressPanel.getVisibility() == View.VISIBLE)) return;
        buttons.setVisibility(on ? View.GONE : View.VISIBLE);
        progressPanel.setVisibility(on ? View.VISIBLE : View.GONE);
        if (on) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LOCKED);
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        }
    }

    private void requestAccess() {
        Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + getPackageName()));
        try {
            startActivity(intent);
        } catch (android.content.ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void chooseFolder() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_TREE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        if (requestCode == REQUEST_ZIP) {
            importPicked(data.getData());
            return;
        }
        if (requestCode != REQUEST_TREE) return;
        String path = pathOfTree(data.getData());
        if (path == null) {
            status.setText(R.string.data_folder_unsupported);
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_DATA_PATH, path).apply();
        refresh();
    }

    /**
     * The file path behind a picked folder. Only the external storage
     * provider's ids map onto paths: "primary:wowee" is the shared storage,
     * "1234-ABCD:wowee" a removable volume.
     */
    private static String pathOfTree(Uri tree) {
        if (tree == null) return null;
        return pathOfDocument(tree.getAuthority(), DocumentsContract.getTreeDocumentId(tree));
    }

    /**
     * The same for one document. The downloads provider also hands out
     * "raw:/storage/..." ids for files it knows the path of.
     */
    private static String pathOfDocument(String authority, String id) {
        if (id == null) return null;
        if ("com.android.providers.downloads.documents".equals(authority) && id.startsWith("raw:")) {
            return id.substring(4);
        }
        if (!"com.android.externalstorage.documents".equals(authority)) return null;
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon);
        String rel = id.substring(colon + 1);
        File base = "primary".equalsIgnoreCase(volume)
                ? Environment.getExternalStorageDirectory()
                : new File("/storage", volume);
        return rel.isEmpty() ? base.getAbsolutePath() : new File(base, rel).getAbsolutePath();
    }

    private void startGame() {
        startActivity(new Intent(this, WoweeActivity.class));
        finish();
    }
}
