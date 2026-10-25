package org.stronghold.port.launcherqa;

import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;

/** Loads the installed, compiled resources using the device's actual icon mask. */
public final class LauncherIconSelfcheck extends Instrumentation {
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            File output = new File(getTargetContext().getFilesDir(), "launcher-icon-qa");
            if (!output.isDirectory() && !output.mkdir()) throw new Exception("Cannot create icon output");
            PackageManager packages = getTargetContext().getPackageManager();
            java.util.List<android.content.pm.ResolveInfo> entries = packages.queryIntentActivities(
                    new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage("org.stronghold.port"), 0);
            if (entries.size() != 1 || !entries.get(0).activityInfo.name.equals("org.stronghold.port.LauncherActivity"))
                throw new Exception("Expected exactly one launcher icon");
            for (String name : new String[] {"LauncherActivity"}) {
                Drawable icon = packages.getActivityInfo(new ComponentName("org.stronghold.port", "org.stronghold.port." + name),
                        PackageManager.MATCH_DISABLED_COMPONENTS).loadIcon(packages);
                if (!(icon instanceof AdaptiveIconDrawable)) throw new Exception(name + " is not adaptive");
                AdaptiveIconDrawable adaptive = (AdaptiveIconDrawable)icon;
                if (adaptive.getBackground() == null || adaptive.getForeground() == null || adaptive.getMonochrome() == null)
                    throw new Exception(name + " has missing layers");
                Bitmap bitmap = Bitmap.createBitmap(216, 216, Bitmap.Config.ARGB_8888);
                icon.setBounds(0, 0, 216, 216); icon.draw(new Canvas(bitmap));
                try (FileOutputStream file = new FileOutputStream(new File(output, name + ".png"))) {
                    if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, file)) throw new Exception("PNG failed");
                }
            }
            result.putString("stream", "One launcher entry and installed AdaptiveIconDrawable: background, foreground, monochrome and device-mask render PASS\n");
            finish(-1, result);
        } catch (Exception error) { result.putString("stream", error.toString()); finish(0, result); }
    }
}
