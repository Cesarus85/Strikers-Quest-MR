package io.github.newcoke.strikers;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputFilter;
import android.text.InputType;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.RelativeSizeSpan;
import android.text.style.StyleSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.List;

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
 *
 * <p>r43: the Version button, Classic (alpha 1.2's game) or Plus (with what was added after it), and
 * why an online match didn't start (the friend on the other Version or another release).
 *
 * <p>r44: Quick match (a queue on the room server pairs you with anyone else searching on the same
 * Version and release, and both get a room code) and the Voice chat switch.
 */
public class LobbyActivity extends Activity {

    // strikers-quest r44: the room server (src/platform/xr_net.cpp kServerHost) and the online protocol
    // (kProtocol there): quick match only pairs headsets that can play each other.
    private static final String SERVER_HOST = "strikers-rooms.strikers-rooms.workers.dev";
    private static final int ONLINE_PROTOCOL = 5;
    private static final int REQUEST_MIC = 44;
    private int quickToken;          // r44: the search running (a newer one, or Cancel, ends it)
    private boolean searching;       // r44: the quick match page is up

    static final String EXTRA_ROOM = "room";
    // strikers-quest r43: the Version the panel shows (StrikersHome.CLASSIC / PLUS), so a game already
    // running on the other one can tell (StrikersActivity.onNewIntent).
    static final String EXTRA_EDITION = "edition";
    // strikers-quest r43: why the last online match didn't start, one line beside the game's log
    // (src/platform/xr_net.cpp writes "edition=..." or "protocol=..."; StrikersActivity this one).
    static final String ONLINE_NOTE = "online-note.txt";
    static final String NOTE_VERSION_CHANGED = "version-changed";
    private static final String PREFS = "lobby";
    private static final String LAST_CODE = "lastCode";
    // No I, O, 0 or 1: a code is read out loud or off a headset.
    private static final String CODE_LETTERS = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    private static final int BG = Color.rgb(16, 20, 28);
    private static final int CARD = Color.rgb(30, 38, 52);
    private static final int ACCENT = Color.rgb(255, 196, 0);
    private static final int WARN = Color.rgb(255, 150, 140);
    private static final int MUTED = Color.rgb(150, 160, 175);

    private LinearLayout page;
    private SharedPreferences prefs;
    private TextView accessNote;  // r39b: under the buttons while the file access switch is off
    private int chooserToken;     // r41: the game file list being filled (a newer one wins)
    private Updater.Release update;  // r41: a newer release on GitHub, null when there is none
    private boolean onHome;          // r41: the home page is showing (the update check redraws it)

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
        // r41: scrolls, for the game file list and a last error on a short panel.
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.setFillViewport(true);
        scroll.addView(page, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);
        showHome();
        checkForUpdate();
    }

    private void showHome() {
        page.removeAllViews();
        onHome = true;
        title("Quest 3 Strikers", 30);
        if (update != null) {
            note(shortName(update) + " is out.").setTextColor(ACCENT);
            page.addView(button("Update now", true, v -> startUpdate()));
        }
        showLastError();
        showOnlineNote();
        note("Quest alpha. Online: both players pick Grudge Match in the same room, on the same Version.");
        page.addView(button("Play offline", true, v -> start("")));
        page.addView(button("Quick match", true, v -> showQuick()));  // r44
        // r44d (Lance: "make sure everyones codes randomize"): a fresh code every time, not the last one.
        page.addView(button("Host an online match", false, v -> showHost(newCode())));
        page.addView(button("Join with a code", false, v -> showJoin()));
        page.addView(button("Voice chat: " + (voiceOn() ? "On" : "Off"), false, v -> toggleVoice()));  // r44
        page.addView(button("Choose game file", false, v -> showChooser(null, null)));
        TextView game = note(gameStatus());
        game.setTextSize(14);
        // strikers-quest r43: Classic or Plus, by the app's own version below.
        page.addView(button("Version: " + editionName(StrikersHome.edition(this)), false, v -> showVersions()));
        TextView version = note(versionText());
        version.setTextSize(12);
        version.setTextColor(MUTED);
    }

    /** strikers-quest r41: GitHub's latest release, once per panel start, off the UI thread. */
    private void checkForUpdate() {
        final String own = versionName();
        new Thread(() -> {
            final Updater.Release r = Updater.newer(own);
            if (r == null) return;
            runOnUiThread(() -> {
                update = r;
                if (onHome) showHome();
            });
        }, "strikers-update-check").start();
    }

    /** strikers-quest r41: download the newer release and hand it to the system installer. */
    private void startUpdate() {
        final Updater.Release r = update;
        if (r == null) return;
        // The system's "install unknown apps" switch for this app, the first time. Where the
        // headset has no such screen, the installer itself asks.
        if (!getPackageManager().canRequestPackageInstalls()) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + getPackageName())));
                showHome();
                note("Allow this app to install its update, then come back here and press Update now"
                        + " again.").setTextColor(WARN);
                return;
            } catch (Exception e) {
                // fall through to the installer
            }
        }
        onHome = false;
        page.removeAllViews();
        title("Updating to " + shortName(r), 24);
        final TextView status = note("Downloading... 0%");
        page.addView(button("Back", false, v -> showHome()));
        new Thread(() -> {
            try {
                File apk = Updater.download(r, new File(getCacheDir(), "update"),
                        pct -> runOnUiThread(() -> status.setText("Downloading... " + pct + "%")));
                runOnUiThread(() -> status.setText("Installing. Confirm on the next screen: the app closes,"
                        + " updates, and you open it again."));
                Updater.install(this, apk);
            } catch (IOException | RuntimeException e) {
                runOnUiThread(() -> status.setText("The update didn't work (" + e.getMessage() + ").\nCheck"
                        + " the Quest's internet connection and try again, or get it from the GitHub page."));
            }
        }, "strikers-update").start();
    }

    /** strikers-quest r41: the system installer's answer (Updater.install). */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent == null || !Updater.ACTION_INSTALL_STATUS.equals(intent.getAction())) return;
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);  // the "Install?" screen
            if (confirm != null) startActivity(confirm);
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            String msg = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            showHome();
            note("The update wasn't installed" + (msg != null ? ": " + msg : ".")).setTextColor(WARN);
        }
    }

    /** "alpha 1.2" out of "Quest 3 Strikers - alpha 1.2". */
    private static String shortName(Updater.Release r) {
        String name = r.name.replaceFirst("(?i)^quest 3 strikers\\s*[-–—:]\\s*", "");
        return name.isEmpty() ? r.tag : name.substring(0, 1).toUpperCase(java.util.Locale.ROOT) + name.substring(1);
    }

    /**
     * strikers-quest r41: the message the game closed with last time (fatal.c writes it; under the
     * immersive view its own message box is never seen), with an OK that clears it.
     */
    private void showLastError() {
        final File f = new File(StrikersHome.homeDir(this), "last-error.txt");
        if (!f.isFile()) return;
        String text = readText(f, 1500);
        String[] lines = text.split("\n");
        StringBuilder shown = new StringBuilder();
        for (int i = 0; i < lines.length && i < 12; i++) shown.append(i > 0 ? "\n" : "").append(lines[i]);
        TextView t = note("The game closed last time with this message:\n\n" + shown);
        t.setTextColor(WARN);
        t.setTextSize(14);
        page.addView(button("OK", false, v -> {
            //noinspection ResultOfMethodCallIgnored
            f.delete();
            showHome();
        }));
    }

    /**
     * strikers-quest r43: why the last online match didn't start, in words, with an OK that clears it.
     * The game comes back here when it refuses one (StrikersActivity), so this is what the player sees
     * first, with the Version button on the same page. Before r43 the reason was only in the log.
     */
    private void showOnlineNote() {
        final File f = new File(StrikersHome.homeDir(this), ONLINE_NOTE);
        if (!f.isFile()) return;
        String why = readText(f, 200);
        String mine = StrikersHome.edition(this);
        String text;
        if (why.startsWith("edition=")) {
            String theirs = StrikersHome.CLASSIC.equals(why.substring(8).trim()) ? StrikersHome.CLASSIC
                    : StrikersHome.PLUS;
            if (theirs.equals(mine)) {  // already switched to theirs
                //noinspection ResultOfMethodCallIgnored
                f.delete();
                return;
            }
            text = "Your friend is playing " + editionName(theirs) + ". You are on " + editionName(mine)
                    + ".\nOne of you: switch Version to match, then start the match again.";
        } else if (why.startsWith(NOTE_VERSION_CHANGED)) {
            text = "The game was still open on the other Version, so it closed. Press the same button"
                    + " again to play " + editionName(mine) + ".";
        } else {
            text = "Your friend has a different release of Quest 3 Strikers, so you can't play online"
                    + " together. Both of you need the same one (Update now, or the GitHub page).";
        }
        TextView t = note(text);
        t.setTextColor(WARN);
        page.addView(button("OK", false, v -> {
            //noinspection ResultOfMethodCallIgnored
            f.delete();
            showHome();
        }));
    }

    /** strikers-quest r43: one line for {@link #showOnlineNote} (StrikersActivity, in the game's process). */
    static void writeOnlineNote(android.content.Context context, String line) {
        try (java.io.OutputStream out = new java.io.FileOutputStream(
                new File(StrikersHome.homeDir(context), ONLINE_NOTE))) {
            out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            // Only the explanation is lost; the panel still opens.
        }
    }

    /** strikers-quest r43: "Classic 1.2" / "Plus 1.3". */
    private String editionName(String edition) {
        return StrikersHome.CLASSIC.equals(edition) ? "Classic 1.2" : plusName();
    }

    /**
     * strikers-quest r43: Plus is this release, numbered the way the releases are (alpha 1.2 was
     * versionName 0.1.2): "0.1.3-dev (r43)" is Plus 1.3.
     */
    private String plusName() {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("^\\d+\\.(\\d+)\\.(\\d+)").matcher(versionName());
        return "Plus " + (m.find() ? m.group(1) + "." + m.group(2) : "1.3");
    }

    /**
     * strikers-quest r43: the Version picker (Lance: "we draw a line after 1.2 to notify versions after
     * that are no longer strikers as they know it"). A tap saves it at once; the game takes it when it
     * starts (StrikersHome.prepare -> STRIKERS_EDITION, src/platform/edition.cpp).
     */
    private void showVersions() {
        page.removeAllViews();
        onHome = false;
        title("Choose your version", 24);
        note("To play online, you and your friend need the same one.");
        final String current = StrikersHome.edition(this);
        page.addView(versionRow(StrikersHome.CLASSIC, "Mario Strikers as you know it", current));
        // The line: everything under it adds to the game.
        TextView label = note("Below this line: not Strikers as you know it");
        label.setTextColor(ACCENT);
        label.setTextSize(15);
        label.setPadding(0, dp(18), 0, dp(6));
        View line = new View(this);
        line.setBackgroundColor(ACCENT);
        page.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(3)));
        page.addView(versionRow(StrikersHome.PLUS, "New: life-size goal scorer, GOAL SCORED BY banner,"
                + " longer goal replay, living crowd", current));
        page.addView(button("Back", false, v -> showHome()));
    }

    /** strikers-quest r43: one Version in the picker, its name large over what it is. */
    private Button versionRow(String edition, String about, String current) {
        String name = editionName(edition) + (edition.equals(current) ? "   (chosen)" : "");
        Button b = row(name + "\n" + about, true, v -> {
            if (!edition.equals(StrikersHome.edition(this))) {
                StrikersHome.chooseEdition(this, edition);
                endRunningGame();
                // A note that said to switch has been acted on.
                //noinspection ResultOfMethodCallIgnored
                new File(StrikersHome.homeDir(this), ONLINE_NOTE).delete();
            }
            showHome();
        });
        SpannableString text = new SpannableString(b.getText());
        text.setSpan(new RelativeSizeSpan(1.35f), 0, name.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        text.setSpan(new StyleSpan(Typeface.BOLD), 0, name.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        b.setText(text);
        return b;
    }

    /**
     * strikers-quest r43: a game left running in the background (the Quest button, then this app
     * again) keeps the Version it started with, and a start from here would only hand it a room
     * (StrikersActivity.onNewIntent). So a Version change ends it: the next start is a fresh game.
     * It is paused behind this panel, as when the system ends a background app for memory.
     */
    private void endRunningGame() {
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> running = am != null ? am.getRunningAppProcesses() : null;
        if (running == null) return;
        String game = getPackageName() + ":game";
        for (ActivityManager.RunningAppProcessInfo p : running) {
            if (game.equals(p.processName)) {
                android.util.Log.i("strikers", "Version changed: ending the game left running (pid " + p.pid + ")");
                android.os.Process.killProcess(p.pid);
            }
        }
    }

    /** strikers-quest r41: which game file a match would start with, for the home page. */
    private String gameStatus() {
        if (!StrikersHome.hasAllFilesAccess()) {
            return "Game file: the app needs file access to look for it.";
        }
        String path = StrikersHome.describeGameData(this);
        if (path == null) return "No game file found yet. Press \"Choose game file\".";
        File f = new File(path);
        if (f.isDirectory()) return "Game: the extracted disc in " + GameFile.shortPath(f);
        GameFile.Found found = GameFile.check(f);
        return (found.usable ? "Game: " + found.label : "Game file problem: " + found.problem)
                + "\n" + GameFile.shortPath(f);
    }

    /** Null when the game has a file it can read; otherwise what is wrong, in one sentence. */
    private String gameProblem() {
        String path = StrikersHome.describeGameData(this);
        if (path == null) return "Your game file wasn't found on this Quest.";
        File f = new File(path);
        if (f.isDirectory()) return null;
        GameFile.Found found = GameFile.check(f);
        return found.usable ? null : GameFile.shortPath(f) + ": " + found.problem;
    }

    /**
     * strikers-quest r41: every .iso, .rvz and .wia on the headset, checked, to tap the one to play.
     * code != null: the match the player asked for, started once a file is picked.
     */
    private void showChooser(String code, String reason) {
        if (!StrikersHome.hasAllFilesAccess() && askForFileAccess(this)) {
            showHome();
            accessNote = note("Turn on \"Allow access to manage all files\" so the game can find your"
                    + " game file, then come back here and press the same button again.");
            return;
        }
        page.removeAllViews();
        onHome = false;
        title("Choose your game file", 24);
        if (reason != null) note(reason).setTextColor(WARN);
        final TextView status = note("Looking for game files on this Quest...");
        final LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setGravity(Gravity.CENTER_HORIZONTAL);
        page.addView(list, wrap());
        page.addView(button("Look again", false, v -> showChooser(code, null)));
        page.addView(button("Back", false, v -> showHome()));
        final int token = ++chooserToken;
        new Thread(() -> {
            final List<GameFile.Found> found = GameFile.scan();
            runOnUiThread(() -> {
                if (token != chooserToken || list.getParent() != page) return;  // left this page
                if (found.isEmpty()) {
                    status.setText("No .iso, .rvz or .wia file found on this Quest.\n\nOn your PC, open"
                            + " SideQuest, click the folder icon, open Download, and drag your game file"
                            + " in. Then press Look again.");
                    return;
                }
                status.setText("Tap your game file:");
                String chosen = GameFile.chosen(this);
                for (GameFile.Found f : found) {
                    String where = GameFile.shortPath(f.file) + " · " + GameFile.size(f.file.length());
                    boolean isChosen = f.file.getAbsolutePath().equals(chosen);
                    String text = f.file.getName() + (isChosen ? "   (chosen)" : "") + "\n"
                            + (f.usable ? f.label : f.label + ": " + f.problem) + "\n" + where;
                    list.addView(row(text, f.usable, f.usable ? v -> {
                        GameFile.choose(this, f.file.getAbsolutePath());
                        if (code != null) start(code); else showHome();
                    } : null));
                }
            });
        }, "strikers-scan").start();
    }

    private void showHost(String code) {
        page.removeAllViews();
        onHome = false;
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
        onHome = false;
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

    /**
     * strikers-quest r44, Quick match (Lance: "a quick match option that would put you in with anyone
     * else online"): wait in the room server's queue for this Version and release (wss://.../quick/
     * P5 or C5) until someone else is there; both get the same fresh room code and start with it, the
     * way Join does.
     */
    private void showQuick() {
        if (!readyToStart(null)) return;  // the game file first: no use matching a game that can't start
        page.removeAllViews();
        onHome = false;
        searching = true;
        title("Quick match", 24);
        final TextView status = note("Looking for another player...");
        status.setTextColor(ACCENT);
        String edition = StrikersHome.edition(this);
        note("You'll be matched with someone playing " + editionName(edition) + " right now. Then both of"
                + " you: Grudge Match, pick your team, start." + (voiceOn() ? "" : "\nVoice chat is off"
                + " (the button on the first page)."));
        page.addView(button("Cancel", false, v -> {
            ++quickToken;
            searching = false;
            showHome();
        }));
        final int token = ++quickToken;
        final String key = (StrikersHome.CLASSIC.equals(edition) ? "C" : "P") + ONLINE_PROTOCOL;
        new Thread(() -> quickLoop(token, key, status), "strikers-quick").start();
    }

    /** strikers-quest r44: the queue, off the UI thread, reconnecting until matched or cancelled. */
    private void quickLoop(int token, String key, TextView status) {
        final byte[] buf = new byte[512];
        int failures = 0;
        while (token == quickToken) {
            final int h = NetWs.open(SERVER_HOST, 443, "/quick/" + key, 6000);
            if (h < 0) {
                failures++;
                runOnUiThread(() -> status.setText("Can't reach the online server. Trying again...\n(Check the"
                        + " Quest's internet connection.)"));
                android.os.SystemClock.sleep(Math.min(10000, 2000L * failures));
                continue;
            }
            failures = 0;
            String code = null;
            long lastPing = android.os.SystemClock.elapsedRealtime();
            while (token == quickToken) {
                final long now = android.os.SystemClock.elapsedRealtime();
                if (now - lastPing >= 5000) {
                    if (!NetWs.sendText(h, "ping")) break;
                    lastPing = now;
                }
                final int n = NetWs.recv(h, buf, 500);
                if (n == -1) break;  // gone: open a new one (the server drops a socket quiet for 20 s)
                if (n < 0) continue;
                final String msg = new String(buf, 0, n, StandardCharsets.UTF_8).trim();
                if (msg.startsWith("match ")) {
                    code = clean(msg.substring(6));
                    break;  // at once: the close behind it can come seconds later
                }
                if (msg.startsWith("waiting ")) {
                    final String count = msg.substring(8).trim();
                    runOnUiThread(() -> {
                        if (token == quickToken) {
                            status.setText("Looking for another player..." + ("1".equals(count) ? ""
                                    : "\n" + count + " players searching"));
                        }
                    });
                }
            }
            NetWs.close(h);
            if (code != null && code.length() >= 3) {
                final String room = code;
                runOnUiThread(() -> {
                    if (token == quickToken) matched(room);
                });
                return;
            }
        }
    }

    /** strikers-quest r44: a player was found; a moment to read what to do, then the game starts. */
    private void matched(String code) {
        searching = false;
        page.removeAllViews();
        title("Player found!", 28);
        TextView t = note("In the game: Grudge Match, pick your team, start. The match begins when you both"
                + " have.\n\n(Your room: " + spaced(code) + ")");
        t.setTextSize(18);
        final int token = ++quickToken;
        page.postDelayed(() -> {
            if (token == quickToken) start(code);
        }, 3000);
    }

    // --- strikers-quest r44: Voice chat, voice.txt in the game's folder (src/platform/xr_voice.cpp reads
    // it at each online match and the pause panel's right stick writes it). Off until turned on here,
    // where Android's microphone question can be asked without stopping a match.

    private File voiceFile() {
        return new File(StrikersHome.homeDir(this), "voice.txt");
    }

    private boolean voiceOn() {
        return readText(voiceFile(), 8).startsWith("1");
    }

    private void writeVoice(boolean on) {
        try (java.io.OutputStream out = new java.io.FileOutputStream(voiceFile())) {
            out.write((on ? "1\n" : "0\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            android.util.Log.w("strikers", "could not write " + voiceFile(), e);
        }
    }

    private void toggleVoice() {
        if (voiceOn()) {
            writeVoice(false);
            showHome();
            return;
        }
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            voiceGranted();
        } else {
            requestPermissions(new String[] {android.Manifest.permission.RECORD_AUDIO}, REQUEST_MIC);
        }
    }

    private void voiceGranted() {
        writeVoice(true);
        // Whether the headset cancels its own speakers' sound out of the microphone (the game and the
        // friend's voice): if not, the friend hears an echo. For the log, to tell from a test.
        android.util.Log.i("strikers", "voice chat on; echo canceller "
                + (android.media.audiofx.AcousticEchoCanceler.isAvailable() ? "available" : "NOT available"));
        showHome();
        note("Voice chat is on: in online matches you hear each other. Turn it off here, or in the game's"
                + " pause screen (right stick down).");
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != REQUEST_MIC) return;
        if (results.length > 0 && results[0] == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            voiceGranted();
        } else {
            writeVoice(false);
            showHome();
            note("Voice chat needs the microphone. Press Voice chat again and choose Allow.").setTextColor(WARN);
        }
    }

    /** strikers-quest r44: the panel goes out of sight (headset off, Quest menu): stop searching. */
    @Override
    protected void onStop() {
        super.onStop();
        if (searching) {
            ++quickToken;
            searching = false;
            showHome();
        }
    }

    /**
     * strikers-quest r44, out of start(): the file access switch and a readable game file, before a
     * match (or a quick-match search) is started. False when it went to ask for one of them instead.
     */
    private boolean readyToStart(String code) {
        // strikers-quest r39b: the disc usually sits in Download, which takes "all files access". A
        // friend who only installed the APK has not given it yet: open that switch from here and
        // stay on this page, rather than start the game only for it to open the switch and close.
        // Not when the disc is in the app's own folder, which needs no permission.
        // r41: asked whenever the disc is not in the app's own folder (a picked file is elsewhere).
        if (!StrikersHome.hasAllFilesAccess()
                && !StrikersHome.hasGameDataInHome(StrikersHome.homeDir(this))
                && askForFileAccess(this)) {
            if (accessNote == null || accessNote.getParent() != page) accessNote = note("");
            accessNote.setText("Turn on \"Allow access to manage all files\" so the game can read your"
                    + " disc file, then come back here and press the same button again.");
            return false;
        }
        // r41: no game file, or one the game cannot read: say so and show the list, rather than
        // start a game that closes at once (alpha 1: "it just closes").
        String problem = gameProblem();
        if (problem != null) {
            showChooser(code, problem);
            return false;
        }
        return true;
    }

    private void start(String code) {
        if (!code.isEmpty()) prefs.edit().putString(LAST_CODE, code).apply();
        if (!readyToStart(code)) return;
        Intent game = new Intent(this, StrikersActivity.class);
        game.putExtra(EXTRA_ROOM, code);
        game.putExtra(EXTRA_EDITION, StrikersHome.edition(this));  // strikers-quest r43
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

    /** r41: one file in the game file list; onClick null = shown but not pickable. */
    private Button row(String text, boolean usable, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(15);
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        b.setTextColor(usable ? Color.WHITE : MUTED);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(12));
        bg.setColor(CARD);
        if (usable) bg.setStroke(dp(2), ACCENT);
        b.setBackground(bg);
        int p = dp(14);
        b.setPadding(p, p, p, p);
        b.setEnabled(onClick != null);
        b.setOnClickListener(onClick);
        // r43b: the panel's width, wrapping (Lance: the Version rows ran past the panel at 620 dp).
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    private String versionText() {
        String v = versionName();
        return v.isEmpty() ? "" : "Version " + v;
    }

    private static String readText(File f, int max) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[max];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8).trim() : "";
        } catch (IOException e) {
            return "";
        }
    }

    private LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
