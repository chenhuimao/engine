// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine.loader;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;
import android.view.WindowManager;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.tracing.Trace;
import io.flutter.BuildConfig;
import io.flutter.FlutterInjector;
import io.flutter.Log;
import io.flutter.embedding.engine.FlutterJNI;
import io.flutter.util.PathUtils;
import io.flutter.view.VsyncWaiter;
import java.io.File;
import java.util.*;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;

/** Finds Flutter resources in an application APK and also loads Flutter's native library. */
public class FlutterLoader {
  private static final String TAG = "FlutterLoader";

  private static final String OLD_GEN_HEAP_SIZE_META_DATA_KEY =
      "io.flutter.embedding.android.OldGenHeapSize";
  private static final String ENABLE_SKPARAGRAPH_META_DATA_KEY =
      "io.flutter.embedding.android.EnableSkParagraph";

  // Must match values in flutter::switches
  static final String AOT_SHARED_LIBRARY_NAME = "aot-shared-library-name";
  static final String AOT_VMSERVICE_SHARED_LIBRARY_NAME = "aot-vmservice-shared-library-name";
  static final String SNAPSHOT_ASSET_PATH_KEY = "snapshot-asset-path";
  static final String VM_SNAPSHOT_DATA_KEY = "vm-snapshot-data";
  static final String ISOLATE_SNAPSHOT_DATA_KEY = "isolate-snapshot-data";
  static final String FLUTTER_ASSETS_DIR_KEY = "flutter-assets-dir";
  static final String AUTOMATICALLY_REGISTER_PLUGINS_KEY = "automatically-register-plugins";

  // Resource names used for components of the precompiled snapshot.
  private static final String DEFAULT_LIBRARY = "libflutter.so";
  private static final String DEFAULT_KERNEL_BLOB = "kernel_blob.bin";
  private static final String VMSERVICE_SNAPSHOT_LIBRARY = "libvmservice_snapshot.so";

  private static FlutterLoader instance;

  /**
   * Creates a {@code FlutterLoader} that uses a default constructed {@link FlutterJNI} and {@link
   * ExecutorService}.
   */
  public FlutterLoader() {
    this(FlutterInjector.instance().getFlutterJNIFactory().provideFlutterJNI());
  }

  /**
   * Creates a {@code FlutterLoader} that uses a default constructed {@link ExecutorService}.
   *
   * @param flutterJNI The {@link FlutterJNI} instance to use for loading the libflutter.so C++
   *     library, setting up the font manager, and calling into C++ initialization.
   */
  public FlutterLoader(@NonNull FlutterJNI flutterJNI) {
    this(flutterJNI, FlutterInjector.instance().executorService());
  }

  /**
   * Creates a {@code FlutterLoader} with the specified {@link FlutterJNI}.
   *
   * @param flutterJNI The {@link FlutterJNI} instance to use for loading the libflutter.so C++
   *     library, setting up the font manager, and calling into C++ initialization.
   * @param executorService The {@link ExecutorService} to use when creating new threads.
   */
  public FlutterLoader(@NonNull FlutterJNI flutterJNI, @NonNull ExecutorService executorService) {
    this.flutterJNI = flutterJNI;
    this.executorService = executorService;
  }

  private boolean initialized = false;
  @Nullable private Settings settings;
  private long initStartTimestampMillis;
  private FlutterApplicationInfo flutterApplicationInfo;
  private FlutterJNI flutterJNI;
  private ExecutorService executorService;

  private static class InitResult {
    final String appStoragePath;
    final String engineCachesPath;
    final String dataDirPath;

    private InitResult(String appStoragePath, String engineCachesPath, String dataDirPath) {
      this.appStoragePath = appStoragePath;
      this.engineCachesPath = engineCachesPath;
      this.dataDirPath = dataDirPath;
    }
  }

  @Nullable Future<InitResult> initResultFuture;

  /**
   * Starts initialization of the native system.
   *
   * @param applicationContext The Android application context.
   */
  public void startInitialization(@NonNull Context applicationContext) {
    startInitialization(applicationContext, new Settings());
  }

  /**
   * Starts initialization of the native system.
   *
   * <p>This loads the Flutter engine's native library to enable subsequent JNI calls. This also
   * starts locating and unpacking Dart resources packaged in the app's APK.
   *
   * <p>Calling this method multiple times has no effect.
   *
   * @param applicationContext The Android application context.
   * @param settings Configuration settings.
   */
  public void startInitialization(@NonNull Context applicationContext, @NonNull Settings settings) {
    // Do not run startInitialization more than once.
    if (this.settings != null) {
      return;
    }
    if (Looper.myLooper() != Looper.getMainLooper()) {
      throw new IllegalStateException("startInitialization must be called on the main thread");
    }

    Trace.beginSection("FlutterLoader#startInitialization");

    try {
      // Ensure that the context is actually the application context.
      final Context appContext = applicationContext.getApplicationContext();

      this.settings = settings;

      initStartTimestampMillis = SystemClock.uptimeMillis();
      flutterApplicationInfo = ApplicationInfoLoader.load(appContext);

      float fps;
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
        final DisplayManager dm = appContext.getSystemService(DisplayManager.class);
        final Display primaryDisplay = dm.getDisplay(Display.DEFAULT_DISPLAY);
        fps = primaryDisplay.getRefreshRate();
      } else {
        fps =
            ((WindowManager) appContext.getSystemService(Context.WINDOW_SERVICE))
                .getDefaultDisplay()
                .getRefreshRate();
      }
      VsyncWaiter.getInstance(fps).init();

      // Use a background thread for initialization tasks that require disk access.
      Callable<InitResult> initTask =
          new Callable<InitResult>() {
            @Override
            public InitResult call() {
              Trace.beginSection("FlutterLoader initTask");

              try {
                ResourceExtractor resourceExtractor = initResources(appContext);

                flutterJNI.loadLibrary();

                // Prefetch the default font manager as soon as possible on a background thread.
                // It helps to reduce time cost of engine setup that blocks the platform thread.
                executorService.execute(() -> flutterJNI.prefetchDefaultFontManager());

                if (resourceExtractor != null) {
                  resourceExtractor.waitForCompletion();
                }

                return new InitResult(
                    PathUtils.getFilesDir(appContext),
                    PathUtils.getCacheDirectory(appContext),
                    PathUtils.getDataDirectory(appContext));
              } finally {
                Trace.endSection();
              }
            }
          };
      initResultFuture = executorService.submit(initTask);
    } finally {
      Trace.endSection();
    }
  }

  /**
   * Blocks until initialization of the native system has completed.
   *
   * <p>Calling this method multiple times has no effect.
   *
   * @param applicationContext The Android application context.
   * @param args Flags sent to the Flutter runtime.
   */
  public void ensureInitializationComplete(
      @NonNull Context applicationContext, @Nullable String[] args) {
    if (initialized) {
      return;
    }
    if (Looper.myLooper() != Looper.getMainLooper()) {
      throw new IllegalStateException(
          "ensureInitializationComplete must be called on the main thread");
    }
    if (settings == null) {
      throw new IllegalStateException(
          "ensureInitializationComplete must be called after startInitialization");
    }
    Trace.beginSection("FlutterLoader#ensureInitializationComplete");

    try {
      InitResult result = initResultFuture.get();

      List<String> shellArgs = new ArrayList<>();
      shellArgs.add("--icu-symbol-prefix=_binary_icudtl_dat");

      shellArgs.add(
          "--icu-native-lib-path="
              + flutterApplicationInfo.nativeLibraryDir
              + File.separator
              + DEFAULT_LIBRARY);
      if (args != null) {
        Collections.addAll(shellArgs, args);
      }

      String kernelPath = null;
      if (BuildConfig.DEBUG || BuildConfig.JIT_RELEASE) {
        String snapshotAssetPath =
            result.dataDirPath + File.separator + flutterApplicationInfo.flutterAssetsDir;
        kernelPath = snapshotAssetPath + File.separator + DEFAULT_KERNEL_BLOB;
        shellArgs.add("--" + SNAPSHOT_ASSET_PATH_KEY + "=" + snapshotAssetPath);
        shellArgs.add("--" + VM_SNAPSHOT_DATA_KEY + "=" + flutterApplicationInfo.vmSnapshotData);
        shellArgs.add(
            "--" + ISOLATE_SNAPSHOT_DATA_KEY + "=" + flutterApplicationInfo.isolateSnapshotData);
      } else {
        shellArgs.add(
            "--" + AOT_SHARED_LIBRARY_NAME + "=" + flutterApplicationInfo.aotSharedLibraryName);

        // Most devices can load the AOT shared library based on the library name
        // with no directory path.  Provide a fully qualified path to the library
        // as a workaround for devices where that fails.
        shellArgs.add(
            "--"
                + AOT_SHARED_LIBRARY_NAME
                + "="
                + flutterApplicationInfo.nativeLibraryDir
                + File.separator
                + flutterApplicationInfo.aotSharedLibraryName);

        // In profile mode, provide a separate library containing a snapshot for
        // launching the Dart VM service isolate.
        if (BuildConfig.PROFILE) {
          shellArgs.add(
              "--" + AOT_VMSERVICE_SHARED_LIBRARY_NAME + "=" + VMSERVICE_SNAPSHOT_LIBRARY);
        }
      }

      shellArgs.add("--cache-dir-path=" + result.engineCachesPath);
      if (flutterApplicationInfo.domainNetworkPolicy != null) {
        shellArgs.add("--domain-network-policy=" + flutterApplicationInfo.domainNetworkPolicy);
      }
      if (settings.getLogTag() != null) {
        shellArgs.add("--log-tag=" + settings.getLogTag());
      }

      ApplicationInfo applicationInfo =
          applicationContext
              .getPackageManager()
              .getApplicationInfo(
                  applicationContext.getPackageName(), PackageManager.GET_META_DATA);
      Bundle metaData = applicationInfo.metaData;
      int oldGenHeapSizeMegaBytes =
          metaData != null ? metaData.getInt(OLD_GEN_HEAP_SIZE_META_DATA_KEY) : 0;
      if (oldGenHeapSizeMegaBytes == 0) {
        // default to half of total memory.
        ActivityManager activityManager =
            (ActivityManager) applicationContext.getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();
        activityManager.getMemoryInfo(memInfo);
        oldGenHeapSizeMegaBytes = (int) (memInfo.totalMem / 1e6 / 2);
      }

      shellArgs.add("--old-gen-heap-size=" + oldGenHeapSizeMegaBytes);

      shellArgs.add("--prefetched-default-font-manager");

      if (metaData != null && metaData.getBoolean(ENABLE_SKPARAGRAPH_META_DATA_KEY)) {
        shellArgs.add("--enable-skparagraph");
      }

      long initTimeMillis = SystemClock.uptimeMillis() - initStartTimestampMillis;

      flutterJNI.init(
          applicationContext,
          shellArgs.toArray(new String[0]),
          kernelPath,
          result.appStoragePath,
          result.engineCachesPath,
          initTimeMillis);

      initialized = true;
    } catch (Exception e) {
      Log.e(TAG, "Flutter initialization failed.", e);
      throw new RuntimeException(e);
    } finally {
      Trace.endSection();
    }
  }

  /**
   * Same as {@link #ensureInitializationComplete(Context, String[])} but waiting on a background
   * thread, then invoking {@code callback} on the {@code callbackHandler}.
   */
  public void ensureInitializationCompleteAsync(
      @NonNull Context applicationContext,
      @Nullable String[] args,
      @NonNull Handler callbackHandler,
      @NonNull Runnable callback) {
    if (Looper.myLooper() != Looper.getMainLooper()) {
      throw new IllegalStateException(
          "ensureInitializationComplete must be called on the main thread");
    }
    if (settings == null) {
      throw new IllegalStateException(
          "ensureInitializationComplete must be called after startInitialization");
    }
    if (initialized) {
      callbackHandler.post(callback);
      return;
    }
    executorService.execute(
        () -> {
          InitResult result;
          try {
            result = initResultFuture.get();
          } catch (Exception e) {
            Log.e(TAG, "Flutter initialization failed.", e);
            throw new RuntimeException(e);
          }
          new Handler(Looper.getMainLooper())
              .post(
                  () -> {
                    ensureInitializationComplete(applicationContext.getApplicationContext(), args);
                    callbackHandler.post(callback);
                  });
        });
  }

  /** Returns whether the FlutterLoader has finished loading the native library. */
  public boolean initialized() {
    return initialized;
  }

  /** Extract assets out of the APK that need to be cached as uncompressed files on disk. */
  private ResourceExtractor initResources(@NonNull Context applicationContext) {
    ResourceExtractor resourceExtractor = null;
    if (BuildConfig.DEBUG || BuildConfig.JIT_RELEASE) {
      final String dataDirPath = PathUtils.getDataDirectory(applicationContext);
      final String packageName = applicationContext.getPackageName();
      final PackageManager packageManager = applicationContext.getPackageManager();
      final AssetManager assetManager = applicationContext.getResources().getAssets();
      resourceExtractor =
          new ResourceExtractor(dataDirPath, packageName, packageManager, assetManager);

      // In debug/JIT mode these assets will be written to disk and then
      // mapped into memory so they can be provided to the Dart VM.
      resourceExtractor
          .addResource(fullAssetPathFrom(flutterApplicationInfo.vmSnapshotData))
          .addResource(fullAssetPathFrom(flutterApplicationInfo.isolateSnapshotData))
          .addResource(fullAssetPathFrom(DEFAULT_KERNEL_BLOB));

      resourceExtractor.start();
    }
    return resourceExtractor;
  }

  @NonNull
  public String findAppBundlePath() {
    return flutterApplicationInfo.flutterAssetsDir;
  }

  /**
   * Returns the file name for the given asset. The returned file name can be used to access the
   * asset in the APK through the {@link android.content.res.AssetManager} API.
   *
   * @param asset the name of the asset. The name can be hierarchical
   * @return the filename to be used with {@link android.content.res.AssetManager}
   */
  @NonNull
  public String getLookupKeyForAsset(@NonNull String asset) {
    return fullAssetPathFrom(asset);
  }

  /**
   * Returns the file name for the given asset which originates from the specified packageName. The
   * returned file name can be used to access the asset in the APK through the {@link
   * android.content.res.AssetManager} API.
   *
   * @param asset the name of the asset. The name can be hierarchical
   * @param packageName the name of the package from which the asset originates
   * @return the file name to be used with {@link android.content.res.AssetManager}
   */
  @NonNull
  public String getLookupKeyForAsset(@NonNull String asset, @NonNull String packageName) {
    return getLookupKeyForAsset("packages" + File.separator + packageName + File.separator + asset);
  }

  /** Returns the configuration on whether flutter engine should automatically register plugins. */
  @NonNull
  public boolean automaticallyRegisterPlugins() {
    return flutterApplicationInfo.automaticallyRegisterPlugins;
  }

  @NonNull
  private String fullAssetPathFrom(@NonNull String filePath) {
    return flutterApplicationInfo.flutterAssetsDir + File.separator + filePath;
  }

  public static class Settings {
    private String logTag;

    @Nullable
    public String getLogTag() {
      return logTag;
    }

    /**
     * Set the tag associated with Flutter app log messages.
     *
     * @param tag Log tag.
     */
    public void setLogTag(String tag) {
      logTag = tag;
    }
  }
}
