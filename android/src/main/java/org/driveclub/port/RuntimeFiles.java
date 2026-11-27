package org.driveclub.port;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.Closeable;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

/** Read-only /app0 overlay backed by the user's persisted document grants. */
final class RuntimeFiles implements Closeable {
    private final Context context;
    private final Uri base, update;
    private final Map<String, Map<String, Node>> directories = new LinkedHashMap<>();
    private final Map<String, ParcelFileDescriptor> retained = new LinkedHashMap<>();

    private static final class Node {
        final Uri tree, uri;
        final String id;
        final boolean directory;
        final long size, modified;
        Node(Uri tree, Uri uri, String id, boolean directory, long size, long modified) {
            this.tree = tree; this.uri = uri; this.id = id; this.directory = directory;
            this.size = size; this.modified = modified;
        }
    }

    RuntimeFiles(Context context, Uri base, Uri update) {
        this.context = context.getApplicationContext();
        this.base = base; this.update = update;
    }

    private static String checked(String path) throws IOException {
        if (path == null || path.indexOf('\0') >= 0 || path.indexOf('\\') >= 0 || path.startsWith("/"))
            throw new IOException("Caminho de conteúdo inválido");
        if (path.isEmpty()) return path;
        for (String part : path.split("/", -1))
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                throw new IOException("Caminho de conteúdo inválido");
        return path;
    }

    private Map<String, Node> children(Node parent) throws IOException {
        if (!parent.directory) return java.util.Collections.emptyMap();
        String key = parent.tree + "\n" + parent.id;
        Map<String, Node> cached = directories.get(key);
        if (cached != null) return cached;
        Map<String, Node> result = new LinkedHashMap<>();
        String[] columns = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED};
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(parent.tree, parent.id);
        try (Cursor rows = context.getContentResolver().query(uri, columns, null, null, null)) {
            if (rows == null) throw new IOException("O provedor não retornou a pasta selecionada");
            while (rows.moveToNext()) {
                String id = rows.getString(0), name = rows.getString(1);
                if (id == null || name == null || name.indexOf('/') >= 0) continue;
                result.put(name, new Node(parent.tree,
                        DocumentsContract.buildDocumentUriUsingTree(parent.tree, id), id,
                        Document.MIME_TYPE_DIR.equals(rows.getString(2)),
                        rows.isNull(3) ? -1 : rows.getLong(3), rows.isNull(4) ? 0 : rows.getLong(4)));
            }
        }
        directories.put(key, result);
        return result;
    }

    private Node find(Uri tree, String path) throws IOException {
        String id = DocumentsContract.getTreeDocumentId(tree);
        Node node = new Node(tree, DocumentsContract.buildDocumentUriUsingTree(tree, id), id, true, 0, 0);
        if (path.isEmpty()) return node;
        for (String part : path.split("/")) {
            Map<String, Node> entries = children(node);
            Node next = entries.get(part);
            if (next == null) {
                // The runtime also supports dumps made on case-insensitive desktop filesystems.
                for (Map.Entry<String, Node> entry : entries.entrySet()) {
                    if (part.equalsIgnoreCase(entry.getKey())) { next = entry.getValue(); break; }
                }
            }
            if (next == null) return null;
            node = next;
        }
        return node;
    }

    private Node merged(String path) throws IOException {
        Node node = find(update, path);
        return node != null ? node : find(base, path);
    }

    private ParcelFileDescriptor open(Node node) throws IOException {
        ParcelFileDescriptor fd = context.getContentResolver().openFileDescriptor(node.uri, "r");
        if (fd == null) throw new IOException("Não foi possível abrir o arquivo selecionado");
        try {
            Os.lseek(fd.getFileDescriptor(), 0, OsConstants.SEEK_CUR);
            return fd;
        } catch (ErrnoException e) {
            fd.close();
            throw new IOException("O provedor precisa permitir acesso direto aos arquivos. Use uma pasta local do aparelho.", e);
        }
    }

    // Called by JNI from guest file threads. Native code owns each detached descriptor.
    synchronized int openFile(String relativePath) throws IOException {
        Node node = merged(checked(relativePath));
        if (node == null) return -OsConstants.ENOENT;
        if (node.directory) return -OsConstants.EISDIR;
        try (ParcelFileDescriptor fd = open(node)) { return fd.detachFd(); }
    }

    synchronized long[] statFile(String relativePath) throws IOException {
        Node node = merged(checked(relativePath));
        if (node == null) return null;
        long size = node.size;
        if (!node.directory && size <= 0) {
            try (ParcelFileDescriptor fd = open(node)) {
                size = Os.fstat(fd.getFileDescriptor()).st_size;
            } catch (ErrnoException e) { throw new IOException("Falha ao consultar o tamanho do arquivo", e); }
        }
        return new long[]{Math.max(0, size), node.modified, node.directory ? 1 : 0};
    }

    synchronized String[] listFiles(String relativeDirectory) throws IOException {
        String path = checked(relativeDirectory);
        Node high = find(update, path), low = find(base, path);
        if (high != null && !high.directory) return null;
        if (high == null && (low == null || !low.directory)) return null;
        Map<String, Node> entries = new TreeMap<>();
        if (low != null && low.directory) entries.putAll(children(low));
        if (high != null) entries.putAll(children(high));
        String[] result = new String[entries.size()];
        int index = 0;
        for (Map.Entry<String, Node> entry : entries.entrySet())
            result[index++] = (entry.getValue().directory ? "d:" : "f:") + entry.getKey();
        return result;
    }

    // Fonts and media libraries use fopen rather than the game's file API. Keep only
    // those descriptors for the lifetime of the isolated runtime process.
    synchronized String resolveFile(String relativePath) throws IOException {
        String path = checked(relativePath);
        ParcelFileDescriptor fd = retained.get(path);
        if (fd == null) {
            Node node = merged(path);
            if (node == null || node.directory) return null;
            fd = open(node);
            retained.put(path, fd);
        }
        return "/proc/self/fd/" + fd.getFd();
    }

    @Override public synchronized void close() throws IOException {
        IOException failure = null;
        for (ParcelFileDescriptor fd : retained.values()) {
            try { fd.close(); } catch (IOException e) { failure = e; }
        }
        retained.clear();
        directories.clear();
        if (failure != null) throw failure;
    }
}
