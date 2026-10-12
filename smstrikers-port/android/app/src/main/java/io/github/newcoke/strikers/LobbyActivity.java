package io.github.newcoke.strikers;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.security.SecureRandom;

/**
 * strikers-quest r39: the panel in front of the game. Play offline, host an online match (a
 * four-letter room code to tell your friend), or join one by typing its code. The game itself is
 * {@link StrikersActivity}, immersive; this is a flat Quest panel that starts it with the room
 * code (extra "room", "" for offline), which the game hands to its network layer (STRIKERS_ROOM).
 * In a room, both players pick Grudge Match and start: the match begins once both are in.
 *
 * <p>r39b: this panel runs in the app's main process and the game in a process of its own
 * (":game", see StrikersActivity.onDestroy). The "all files access" switch is asked for from here,
 * before the game is started.
 */
public class LobbyActivity extends Activity {

    static final String EXTRA_ROOM = "room";
    private static final String PREFS = "lobby";
    private static final String LAST_CODE = "lastCode";
    // strikers-quest r39b: the code Host showed last, so it shows the same one after a relaunch.
    private static final String LAST_HOSTED = "lastHosted";
    // No I, O, 0 or 1: a code is read out loud or off a headset.
    private static final String CODE_LETTERS = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    private static final int BG = Color.rgb(16, 20, 28);
    private static final int CARD = Color.rgb(30, 38, 52);
    private static final int ACCENT = Color.rgb(255, 196, 0);

    private LinearLayout page;
    private SharedPreferences prefs;
    private TextView accessNote;  // r39b: under the buttons while the file access switch is off

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER_HORIZONTAL);
        page.setBackgroundColor(BG);
        int pad = dp(28);
        page.setPadding(pad, pad, pad, pad);
        setContentView(page);
        showHome();
    }

    private void showHome() {
        page.removeAllViews();
        title("Quest 3 Strikers", 30);
        note("Quest alpha. Online: both players pick Grudge Match in the same room.");
        page.addView(button("Play offline", true, v -> start("")));
        page.addView(button("Host an online match", false, v -> showHost(hostedCode())));
        page.addView(button("Join with a code", false, v -> showJoin()));
    }

    /** strikers-quest r39b: the code Host showed last, or a fresh one the first time. */
    private String hostedCode() {
        String code = clean(prefs.getString(LAST_HOSTED, ""));
        return code.length() >= 3 ? code : newCode();
    }

    private void showHost(String code) {
        // strikers-quest r39b: remembered as soon as it is shown, since that is when it gets read
        // out to the friend. Host shows it again until "New code".
        prefs.edit().putString(LAST_HOSTED, code).apply();
        page.removeAllViews();
        title("Your room code", 24);
        TextView big = new TextView(this);
        big.setText(spaced(code));
        big.setTextColor(ACCENT);
        big.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        big.setTextSize(64);
        big.setGravity(Gravity.CENTER);
        big.setPadding(0, dp(8), 0, dp(8));
        page.addView(big, wrap());
        note("Tell your friend this code. Both of you: Grudge Match, pick your team, start.");
        page.addView(button("Start", true, v -> start(code)));
        page.addView(button("New code", false, v -> showHost(newCode())));
        page.addView(button("Back", false, v -> showHome()));
    }

    private void showJoin() {
        page.removeAllViews();
        title("Join a room", 24);
        note("Type the code your friend sees.");
        EditText field = new EditText(this);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        field.setFilters(new InputFilter[] {new InputFilter.AllCaps(), new InputFilter.LengthFilter(8)});
        field.setTextSize(40);
        field.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        field.setTextColor(Color.WHITE);
        field.setHintTextColor(Color.GRAY);
        field.setHint("CODE");
        field.setGravity(Gravity.CENTER);
        field.setImeOptions(EditorInfo.IME_ACTION_GO);
        field.setSingleLine(true);
        field.setText(prefs.getString(LAST_CODE, ""));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(360), ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        page.addView(field, lp);
        TextView problem = note("");
        Runnable join = () -> {
            String code = clean(field.getText().toString());
            if (code.length() < 3) {
                problem.setText("A code is at least 3 letters or numbers.");
                return;
            }
            start(code);
        };
        field.setOnEditorActionListener((v, actionId, event) -> {
            join.run();
            return true;
        });
        page.addView(button("Join", true, v -> join.run()));
        page.addView(button("Back", false, v -> showHome()));
        field.requestFocus();
    }

    private void start(String code) {
        if (!code.isEmpty()) prefs.edit().putString(LAST_CODE, code).apply();
        // strikers-quest r39b: the disc usually sits in Download, which takes "all files access". A
        // friend who only installed the APK has not given it yet: open that switch from here and
        // stay on this page, rather than start the game only for it to open the switch and close.
        // Not when the disc is in the app's own folder, which needs no permission.
        if (!StrikersHome.hasAllFilesAccess() && !StrikersHome.hasGameData(this)
                && askForFileAccess(this)) {
            if (accessNote == null || accessNote.getParent() != page) accessNote = note("");
            accessNote.setText("Turn on \"Allow access to manage all files\" so the game can read your"
                    + " disc file, then come back here and press the same button again.");
            return;
        }
        Intent game = new Intent(this, StrikersActivity.class);
        game.putExtra(EXTRA_ROOM, code);
        startActivity(game);
        finish();
    }

    /**
     * strikers-quest: the "all files access" switch for this app; false when the system has no such
     * screen. r39b: moved here from StrikersActivity, which still uses it when started directly.
     */
    static boolean askForFileAccess(Activity activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false;
        try {
            activity.startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + activity.getPackageName())));
            return true;
        } catch (Exception e) {
            try {
                activity.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                return true;
            } catch (Exception e2) {
                return false;
            }
        }
    }

    private static String newCode() {
        SecureRandom r = new SecureRandom();
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 4; i++) sb.append(CODE_LETTERS.charAt(r.nextInt(CODE_LETTERS.length())));
        return sb.toString();
    }

    static String clean(String s) {
        StringBuilder sb = new StringBuilder();
        for (char c : s.toUpperCase().toCharArray()) {
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) sb.append(c);
            if (sb.length() == 8) break;
        }
        return sb.toString();
    }

    private static String spaced(String code) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < code.length(); i++) {
            if (i > 0) sb.append(' ');
            sb.append(code.charAt(i));
        }
        return sb.toString();
    }

    private void title(String text, int sp) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(Color.WHITE);
        t.setTextSize(sp);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setGravity(Gravity.CENTER);
        page.addView(t, wrap());
    }

    private TextView note(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(Color.rgb(190, 200, 215));
        t.setTextSize(16);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, dp(6), 0, dp(14));
        page.addView(t, wrap());
        return t;
    }

    private Button button(String text, boolean primary, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(20);
        b.setTextColor(primary ? Color.BLACK : Color.WHITE);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(14));
        bg.setColor(primary ? ACCENT : CARD);
        b.setBackground(bg);
        b.setOnClickListener(onClick);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(360), dp(64));
        lp.topMargin = dp(10);
        b.setLayoutParams(lp);
        return b;
    }

    private LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
