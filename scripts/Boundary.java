// For a list of function entries: name, size, strings referenced (directly or via one callee level).
// Args: <in list> <out tsv>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class Boundary extends GhidraScript {
    List<String> strings(Function f) {
        List<String> out = new ArrayList<>();
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true))
            for (Reference r : ins.getReferencesFrom()) {
                Data d = getDataAt(r.getToAddress());
                if (d != null && d.hasStringValue()) {
                    String s = StringDataInstance.getStringDataInstance(d).getStringValue();
                    if (s != null && s.length() > 2) out.add(s.replace("\t", " ").replace("\n", " "));
                }
            }
        return out;
    }
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[1]));
        for (String line : Files.readAllLines(Paths.get(a[0]))) {
            String[] p = line.split("\t");
            Function f = getFunctionAt(toAddr(Long.parseLong(p[0], 16)));
            if (f == null) continue;
            List<String> s = strings(f);
            if (s.isEmpty()) for (Function c : f.getCalledFunctions(monitor)) { s.addAll(strings(c)); if (s.size() > 3) break; }
            StringBuilder imports = new StringBuilder();
            for (Function c : f.getCalledFunctions(monitor)) if (c.isExternal() || c.isThunk()) imports.append(c.getName()).append(' ');
            out.println(p[0] + "\t" + p[1] + "\t" + f.getName(true) + "\t" + f.getBody().getNumAddresses() + "\t" +
                        imports + "\t" + String.join(" | ", s.subList(0, Math.min(4, s.size()))));
        }
        out.close();
        println("BOUNDARY done");
    }
}
