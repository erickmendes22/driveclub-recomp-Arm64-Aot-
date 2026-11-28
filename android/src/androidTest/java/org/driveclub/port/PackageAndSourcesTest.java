package org.driveclub.port;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.SharedPreferences;
import android.net.Uri;
import android.test.InstrumentationTestCase;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;
import java.util.stream.Stream;

/** Real JNI extraction and source publication, restricted to this test's private namespace. */
public final class PackageAndSourcesTest extends InstrumentationTestCase {
    private static final String BASE_EXECUTABLE = "BASE-EXECUTABLE-FIXTURE\n";
    private static final String BASE_ARCHIVE = "BASE-ARCHIVE-FIXTURE\n";
    private static final String UPDATE_EXECUTABLE = "UPDATE-EXECUTABLE-FIXTURE\n";
    private static final String UPDATE_PAYLOAD = "UPDATE-PAYLOAD\n";
    private static final String SAVE = "progress that must survive installations\n";
    private Path sandbox, installed, save;
    private IsolatedContext context;
    private GameLibrary library;
    private PortLog log;

    @Override protected void setUp() throws Exception {
        super.setUp();
        Context target = getInstrumentation().getTargetContext();
        sandbox = Files.createTempDirectory(target.getCacheDir().toPath(), "driveclub-package-test-");
        context = new IsolatedContext(target, sandbox);
        installed = context.getNoBackupFilesDir().toPath().resolve("driveclub/installed");
        save = context.getNoBackupFilesDir().toPath().resolve("driveclub/user/savedata/CUSA00093/progress.dat");
        Files.createDirectories(save.getParent());
        Files.write(save, SAVE.getBytes(StandardCharsets.UTF_8));
        library = new GameLibrary(context);
        log = new PortLog(context);
    }

    @Override protected void tearDown() throws Exception {
        try {
            if (context != null) context.removeTestPreferences();
            if (sandbox != null) deleteTree(sandbox);
        } finally { super.tearDown(); }
    }

    public void testNativeBaseAndUpdatePublicationPreservesArchiveAndSaves() throws Exception {
        PkgInstaller.Result base = installFixture("driveclub-base-test.pkg", false, "01.00");
        assertNull("Extraction must not publish a partially selected game", library.folder(GameLibrary.BASE));
        assertEquals(BASE_EXECUTABLE, read(base.root.resolve("eboot.bin")));
        assertEquals(BASE_ARCHIVE, read(base.root.resolve("game000.dat")));
        library.publish(base, log);
        Uri baseUri = Uri.fromFile(base.root.toFile());
        assertEquals(baseUri, library.folder(GameLibrary.BASE));
        assertNull(library.folder(GameLibrary.UPDATE));
        assertEquals(baseUri, library.effectiveUpdate());
        assertEquals(SAVE, read(save));

        PkgInstaller.Result update = installFixture("driveclub-update-test.pkg", true, "01.28");
        assertNull("The old selection must remain active until publication", library.folder(GameLibrary.UPDATE));
        assertFalse("The update fixture contains no base archive", Files.exists(update.root.resolve("game000.dat")));
        library.publish(update, log);
        Uri updateUri = Uri.fromFile(update.root.toFile());
        assertEquals(baseUri, library.folder(GameLibrary.BASE));
        assertEquals(updateUri, library.folder(GameLibrary.UPDATE));
        assertEquals(updateUri, library.effectiveUpdate());
        assertTrue(library.matches(baseUri, updateUri));
        assertFalse(library.matches(baseUri, baseUri));
        assertEquals("Publishing an overlay must not modify the base executable", BASE_EXECUTABLE, read(base.root.resolve("eboot.bin")));
        assertEquals(BASE_ARCHIVE, read(base.root.resolve("game000.dat")));
        assertOverlay();
        assertEquals(SAVE, read(save));
        noStaging();

        // A replacement base clears the selected overlay. Previous generations remain
        // available until real local preparation succeeds, while saves stay outside both.
        PkgInstaller.Result replacement = installFixture("driveclub-base-test.pkg", false, "01.00");
        library.publish(replacement, log);
        assertTrue("Keep the previous base until preparation succeeds", Files.exists(base.root));
        assertTrue("Keep the previous update until preparation succeeds", Files.exists(update.root));
        assertEquals(Uri.fromFile(replacement.root.toFile()), library.folder(GameLibrary.BASE));
        assertNull(library.folder(GameLibrary.UPDATE));
        assertEquals(SAVE, read(save));
        try (RuntimeFiles files = new RuntimeFiles(context, library.folder(GameLibrary.BASE), library.effectiveUpdate())) {
            assertEquals(BASE_EXECUTABLE, read(files, "eboot.bin"));
            assertEquals(BASE_ARCHIVE, read(files, "game000.dat"));
        }
        noStaging();
    }

    public void testCancelledAndCorruptExtractionKeepPublishedGameAndRemoveStaging() throws Exception {
        PkgInstaller.Result base = installFixture("driveclub-base-test.pkg", false, "01.00");
        library.publish(base, log);
        PkgInstaller.Result update = installFixture("driveclub-update-test.pkg", true, "01.28");
        library.publish(update, log);
        Map<String, ?> preferences = new HashMap<>(context.getSharedPreferences("game-library", Context.MODE_PRIVATE).getAll());
        Set<String> existing = installedPaths();

        Path interrupted = Files.createDirectories(installed.resolve(".pkg-staging-interrupted"));
        Files.write(interrupted.resolve("partial.bin"), new byte[]{1,2,3});
        Uri updateSource = fixture("driveclub-update-test.pkg");
        PkgInstaller.Plan updatePlan = PkgInstaller.inspect(context, updateSource);
        AtomicLong cancelledAfterBytes = new AtomicLong();
        try {
            PkgInstaller.install(context, updateSource, updatePlan, (done, total, path) -> {
                cancelledAfterBytes.set(Math.max(cancelledAfterBytes.get(), done));
                return done == 0;
            });
            fail("Cancellation after a native write must stop installation");
        } catch (PkgInstaller.CancelledException expected) {
            assertTrue("Cancellation must reach native extraction, after metadata was written", cancelledAfterBytes.get() > 0);
        }
        assertFalse("An interrupted process must not leave orphan staging on the next installation", Files.exists(interrupted));
        assertPublishedUnchanged(preferences, existing, base);

        Uri corruptSource = fixture("driveclub-corrupt-test.pkg");
        PkgInstaller.Plan corruptPlan = PkgInstaller.inspect(context, corruptSource);
        assertEquals("The corrupt fixture retains valid PKG metadata", "CUSA00093", corruptPlan.titleId);
        AtomicLong corruptProgress = new AtomicLong();
        try {
            PkgInstaller.install(context, corruptSource, corruptPlan, (done, total, path) -> {
                corruptProgress.set(Math.max(corruptProgress.get(), done));
                return true;
            });
            fail("Corrupt compressed file data must fail extraction");
        } catch (IOException expected) {
            assertFalse("Corruption is an extraction failure, not user cancellation", expected instanceof PkgInstaller.CancelledException);
            assertTrue("Corrupt data must be detected during extraction after staging writes", corruptProgress.get() > 0);
        }
        assertPublishedUnchanged(preferences, existing, base);
    }

    public void testUnifiedFolderNeedsNoSeparateUpdateAndUsesTheSameRuntimeAccess() throws Exception {
        Path merged = Files.createDirectories(installed.resolve("unified-fixture"));
        Files.write(merged.resolve("game000.dat"), BASE_ARCHIVE.getBytes(StandardCharsets.UTF_8));
        Files.write(merged.resolve("eboot.bin"), UPDATE_EXECUTABLE.getBytes(StandardCharsets.UTF_8));
        Files.write(merged.resolve("update-only.bin"), UPDATE_PAYLOAD.getBytes(StandardCharsets.UTF_8));
        Uri folder = Uri.fromFile(merged.toFile());
        // Arrange the persisted state of a previously selected, already merged folder.
        // This test stays within installed/ to satisfy the file-URI boundary; the
        // optional-update logic is shared with SAF folders selected by the launcher.
        assertTrue(context.getSharedPreferences("game-library", Context.MODE_PRIVATE).edit()
                .putString(GameLibrary.BASE, folder.toString()).remove(GameLibrary.UPDATE).commit());
        assertEquals(folder, library.folder(GameLibrary.BASE));
        assertNull(library.folder(GameLibrary.UPDATE));
        assertEquals(folder, library.effectiveUpdate());
        assertTrue(library.matches(folder, folder));

        try (RuntimeFiles explicit = new RuntimeFiles(context, folder, library.effectiveUpdate());
             RuntimeFiles optional = new RuntimeFiles(context, folder, null)) {
            for (RuntimeFiles files : new RuntimeFiles[]{explicit, optional}) {
                assertEquals(UPDATE_EXECUTABLE, read(files, "eboot.bin"));
                assertEquals(BASE_ARCHIVE, read(files, "game000.dat"));
                assertEquals(UPDATE_PAYLOAD, read(files, "update-only.bin"));
                assertEquals(BASE_ARCHIVE.getBytes(StandardCharsets.UTF_8).length, files.statFile("game000.dat")[0]);
                assertTrue(Arrays.asList(files.listFiles("")).contains("f:game000.dat"));
            }
        }
        assertEquals(SAVE, read(save));
    }

    private void assertPublishedUnchanged(Map<String, ?> preferences, Set<String> existing, PkgInstaller.Result base) throws Exception {
        assertEquals(preferences, context.getSharedPreferences("game-library", Context.MODE_PRIVATE).getAll());
        assertEquals("Only unpublished staging may be removed", existing, installedPaths());
        assertEquals(BASE_EXECUTABLE, read(base.root.resolve("eboot.bin")));
        assertEquals(BASE_ARCHIVE, read(base.root.resolve("game000.dat")));
        assertOverlay();
        assertEquals(SAVE, read(save));
        noStaging();
    }

    private void assertOverlay() throws Exception {
        try (RuntimeFiles files = new RuntimeFiles(context, library.folder(GameLibrary.BASE), library.effectiveUpdate())) {
            assertEquals(UPDATE_EXECUTABLE, read(files, "eboot.bin"));
            assertEquals(BASE_ARCHIVE, read(files, "game000.dat"));
            assertEquals(UPDATE_PAYLOAD, read(files, "update-only.bin"));
            Set<String> listing = new HashSet<>(Arrays.asList(files.listFiles("")));
            assertTrue(listing.contains("f:eboot.bin"));
            assertTrue(listing.contains("f:game000.dat"));
            assertTrue(listing.contains("f:update-only.bin"));
        }
    }

    private PkgInstaller.Result installFixture(String asset, boolean update, String version) throws Exception {
        Uri source = fixture(asset);
        PkgInstaller.Plan plan = PkgInstaller.inspect(context, source);
        assertEquals("CUSA00093", plan.titleId);
        assertEquals(version, plan.version);
        assertEquals(update, plan.isUpdate);
        assertTrue(plan.packageBytes > 0 && plan.extractedBytes > 0);
        AtomicLong progress = new AtomicLong();
        PkgInstaller.Result result = PkgInstaller.install(context, source, plan, (done, total, path) -> {
            progress.set(Math.max(progress.get(), done));
            return true;
        });
        assertEquals(plan.extractedBytes, progress.get());
        assertEquals(plan.extractedBytes, result.extractedBytes);
        assertEquals(version, result.version);
        assertEquals(update, result.isUpdate);
        assertTrue(result.root.startsWith(installed));
        assertTrue(Files.isDirectory(result.root));
        return result;
    }

    private Uri fixture(String asset) throws IOException {
        Path copy = context.getCacheDir().toPath().resolve(asset);
        if (!Files.exists(copy)) {
            try (InputStream source = getInstrumentation().getContext().getAssets().open(asset)) {
                Files.copy(source, copy);
            }
        }
        return Uri.fromFile(copy.toFile());
    }
    private static String read(Path file) throws IOException {
        return new String(Files.readAllBytes(file), StandardCharsets.UTF_8);
    }
    private static String read(RuntimeFiles files, String path) throws IOException {
        try (InputStream input = files.openInput(path); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] data = new byte[4096];
            int count;
            while ((count = input.read(data)) != -1) output.write(data, 0, count);
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        }
    }
    private Set<String> installedPaths() throws IOException {
        Set<String> paths = new TreeSet<>();
        if (Files.exists(installed)) try (Stream<Path> entries = Files.walk(installed)) {
            entries.skip(1).forEach(path -> paths.add(installed.relativize(path).toString()));
        }
        return paths;
    }
    private void noStaging() throws IOException {
        for (String path : installedPaths()) assertFalse("Unpublished staging remains: " + path, path.startsWith(".pkg-staging-"));
    }
    private static void deleteTree(Path root) throws IOException {
        if (!Files.exists(root)) return;
        Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                Files.delete(file); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path directory, IOException failure) throws IOException {
                if (failure != null) throw failure;
                Files.delete(directory); return FileVisitResult.CONTINUE;
            }
        });
    }

    private static final class IsolatedContext extends ContextWrapper {
        private final Path root;
        private final String namespace = "package_sources_test_" + UUID.randomUUID() + "_";
        private final Set<String> preferences = new HashSet<>();
        IsolatedContext(Context base, Path root) throws IOException {
            super(base); this.root = root;
            Files.createDirectories(root.resolve("no-backup"));
            Files.createDirectories(root.resolve("files"));
            Files.createDirectories(root.resolve("cache"));
        }
        @Override public Context getApplicationContext() { return this; }
        @Override public File getDataDir() { return root.toFile(); }
        @Override public File getNoBackupFilesDir() { return root.resolve("no-backup").toFile(); }
        @Override public File getFilesDir() { return root.resolve("files").toFile(); }
        @Override public File getCacheDir() { return root.resolve("cache").toFile(); }
        @Override public SharedPreferences getSharedPreferences(String name, int mode) {
            String isolated = namespace + name;
            preferences.add(isolated);
            return getBaseContext().getSharedPreferences(isolated, mode);
        }
        void removeTestPreferences() {
            for (String name : preferences) getBaseContext().deleteSharedPreferences(name);
            preferences.clear();
        }
    }
}
