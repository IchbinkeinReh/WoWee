package com.wowee.client;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
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
 * This screen is skipped when access is granted and the folder holds data. The
 * launcher shortcut "Data folder" opens it regardless, to choose again.
 */
public class DataFolderActivity extends Activity {

    static final String EXTRA_CHOOSE = "com.wowee.client.CHOOSE_FOLDER";

    private static final String PREFS = "wowee";
    private static final String KEY_DATA_PATH = "dataPath";
    private static final int REQUEST_TREE = 1;

    private boolean choosing;
    private TextView status;
    private Button primary;

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        choosing = getIntent().getBooleanExtra(EXTRA_CHOOSE, false);
        buildView();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Back from the settings screen, the picker, or the player copying data
        // in: the state may have changed under this screen either way.
        if (!choosing && haveAccess() && hasGameData(dataDir(this))) {
            startGame();
            return;
        }
        refresh();
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

        primary = new Button(this);
        layout.addView(primary, buttonParams());

        Button choose = new Button(this);
        choose.setText(R.string.data_folder_choose);
        choose.setOnClickListener(v -> chooseFolder());
        layout.addView(choose, buttonParams());

        Button reset = new Button(this);
        reset.setText(R.string.data_folder_default);
        reset.setOnClickListener(v -> {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(KEY_DATA_PATH).apply();
            refresh();
        });
        layout.addView(reset, buttonParams());

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
        if (!haveAccess()) {
            status.setText(getString(R.string.data_folder_need_access, folder.getAbsolutePath()));
            primary.setText(R.string.data_folder_grant);
            primary.setOnClickListener(v -> requestAccess());
            return;
        }
        boolean found = hasGameData(dataDir(this));
        status.setText(getString(found ? R.string.data_folder_found : R.string.data_folder_missing,
                folder.getAbsolutePath()));
        primary.setText(R.string.data_folder_start);
        primary.setOnClickListener(v -> startGame());
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
        if (requestCode != REQUEST_TREE || resultCode != RESULT_OK || data == null) return;
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
        if (tree == null || !"com.android.externalstorage.documents".equals(tree.getAuthority())) {
            return null;
        }
        String id = DocumentsContract.getTreeDocumentId(tree);
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
