import it.unimi.dsi.webgraph.ImmutableGraph;
import it.unimi.dsi.webgraph.NodeIterator;

import java.io.BufferedOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;

/** Streams a LAW/WebGraph graph to a SNAP-style tab-separated edge list. */
public final class ConvertWebGraphToTxt {
    private static final int OUTPUT_BUFFER_BYTES = 16 * 1024 * 1024;
    private static final int PROGRESS_NODES = 250_000;

    private ConvertWebGraphToTxt() {}

    private static void writeNonNegativeInt(OutputStream output, int value) throws IOException {
        byte[] digits = new byte[10];
        int end = digits.length;
        do {
            digits[--end] = (byte) ('0' + value % 10);
            value /= 10;
        } while (value != 0);
        output.write(digits, end, digits.length - end);
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            System.err.println("Usage: ConvertWebGraphToTxt BASENAME OUTPUT.txt");
            System.exit(2);
        }

        String basename = Path.of(args[0]).toAbsolutePath().normalize().toString();
        Path output = Path.of(args[1]).toAbsolutePath().normalize();
        Path partial = output.resolveSibling(output.getFileName() + ".part");
        if (Files.exists(output) || Files.exists(partial)) {
            throw new IOException("Refusing to replace existing output or partial output: " + output);
        }
        Files.createDirectories(output.getParent());

        ImmutableGraph graph = ImmutableGraph.loadOffline(basename);
        int numNodes = graph.numNodes();
        long numArcs = graph.numArcs();
        System.err.printf(
                Locale.ROOT,
                "Loaded %s: %,d nodes, %,d arcs%n",
                basename,
                numNodes,
                numArcs);

        long started = System.nanoTime();
        long emitted = 0;
        try (OutputStream outputStream = new BufferedOutputStream(
                Files.newOutputStream(partial), OUTPUT_BUFFER_BYTES)) {
            String header =
                    "# Directed graph converted from LAW WebGraph: " + Path.of(basename).getFileName() + "\n"
                    + "# Nodes: " + numNodes + " Edges: " + numArcs + "\n"
                    + "# FromNodeId\tToNodeId\n";
            outputStream.write(header.getBytes(StandardCharsets.US_ASCII));

            NodeIterator nodes = graph.nodeIterator();
            while (nodes.hasNext()) {
                int source = nodes.nextInt();
                int degree = nodes.outdegree();
                int[] successors = nodes.successorArray();
                for (int index = 0; index < degree; index++) {
                    writeNonNegativeInt(outputStream, source);
                    outputStream.write('\t');
                    writeNonNegativeInt(outputStream, successors[index]);
                    outputStream.write('\n');
                    emitted++;
                }

                int visited = source + 1;
                if (visited % PROGRESS_NODES == 0 || visited == numNodes) {
                    double elapsed = (System.nanoTime() - started) / 1e9;
                    System.err.printf(
                            Locale.ROOT,
                            "Nodes %,d/%,d; edges %,d/%,d; %,.0f edges/s; %.1fs%n",
                            visited,
                            numNodes,
                            emitted,
                            numArcs,
                            emitted / elapsed,
                            elapsed);
                }
            }
        }

        if (emitted != numArcs) {
            throw new IOException(
                    "Edge-count mismatch: emitted " + emitted + ", graph reports " + numArcs
                    + "; partial output retained at " + partial);
        }
        Files.move(partial, output, StandardCopyOption.ATOMIC_MOVE);
        double elapsed = (System.nanoTime() - started) / 1e9;
        System.err.printf(
                Locale.ROOT,
                "Complete: %s (%.2f GB) in %.1fs%n",
                output,
                Files.size(output) / 1e9,
                elapsed);
    }
}
