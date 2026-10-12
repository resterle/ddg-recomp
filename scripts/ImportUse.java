// For each function in a list, the imported (external) functions it calls. Args: <in list> <out tsv>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;

public class ImportUse extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[1]));
        for (String line : Files.readAllLines(Paths.get(a[0]))) {
            Function f = getFunctionAt(toAddr(Long.parseLong(line.trim(), 16)));
            if (f == null) continue;
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (Reference r : ins.getReferencesFrom()) {
                    Symbol s = getSymbolAt(r.getToAddress());
                    Function t = getFunctionAt(r.getToAddress());
                    String name = null;
                    if (t != null && t.isThunk() && t.getThunkedFunction(true).isExternal()) name = t.getThunkedFunction(true).getName(true);
                    else if (t != null && t.isExternal()) name = t.getName(true);
                    else if (s != null && s.isExternal()) name = s.getName(true);
                    else if (s != null && s.getSymbolType() == SymbolType.LABEL && s.getParentNamespace().isExternal()) name = s.getName(true);
                    if (name == null) {
                        // pointer to an IAT slot: [addr] whose data references an external
                        for (Reference r2 : currentProgram.getReferenceManager().getReferencesFrom(r.getToAddress())) {
                            Symbol s2 = getSymbolAt(r2.getToAddress());
                            if (s2 != null && s2.isExternal()) name = s2.getName(true);
                        }
                    }
                    if (name != null) out.println(f.getEntryPoint() + "\t" + name);
                }
            }
        }
        out.close();
        println("IMPORTUSE done");
    }
}
