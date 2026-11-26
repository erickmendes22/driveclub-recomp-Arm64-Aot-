package org.driveclub.port;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** Driveclub's base/update folder flow using persisted, read-only document grants. */
final class GameLibrary {
    static final String BASE = "base", UPDATE = "update";
    private final Context context;
    private final SharedPreferences prefs;

    GameLibrary(Context context) {
        this.context = context.getApplicationContext();
        prefs = context.getSharedPreferences("game-library", Context.MODE_PRIVATE);
    }

    Uri folder(String key) {
        String value = prefs.getString(key, null);
        return value == null ? null : Uri.parse(value);
    }

    void select(String key, Uri tree) {
        context.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        Uri old = folder(key);
        prefs.edit().putString(key, tree.toString()).apply();
        String other = BASE.equals(key) ? UPDATE : BASE;
        if (old != null && !old.equals(tree) && !old.equals(folder(other))) {
            try {
                context.getContentResolver().releasePersistableUriPermission(old, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) { /* A provider can revoke a grant itself. */ }
        }
    }

    private Uri child(Uri tree, String parent, String name, boolean directory) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        String[] columns = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE};
        try (Cursor rows = context.getContentResolver().query(children, columns, null, null, null)) {
            if (rows == null) throw new IOException();
            while (rows.moveToNext()) {
                if (name.equals(rows.getString(1)) && directory == Document.MIME_TYPE_DIR.equals(rows.getString(2))) {
                    return DocumentsContract.buildDocumentUriUsingTree(tree, rows.getString(0));
                }
            }
        }
        return null;
    }

    String summary(String key) throws IOException {
        Uri tree = folder(key);
        if (tree == null) return BASE.equals(key) ? "Selecione a pasta que contém game000.dat." :
                "Selecione a pasta que contém sce_sys.";
        String root = DocumentsContract.getTreeDocumentId(tree);
        if (BASE.equals(key)) return child(tree, root, "game000.dat", false) == null ?
                "game000.dat ausente na pasta selecionada." : "game000.dat encontrado.";
        Uri sys = child(tree, root, "sce_sys", true);
        Uri sfo = sys == null ? null : child(tree, DocumentsContract.getDocumentId(sys), "param.sfo", false);
        if (sfo == null) return "sce_sys/param.sfo ausente na pasta selecionada.";
        return inspectFile(sfo);
    }

    String inspectFile(Uri uri) throws IOException {
        try (InputStream input = context.getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException();
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] block = new byte[4096];
            int count;
            while ((count = input.read(block)) != -1) {
                if (bytes.size() + count > 65536) throw new IOException();
                bytes.write(block, 0, count);
            }
            return PortActivity.inspect(bytes.toByteArray());
        }
    }
}
