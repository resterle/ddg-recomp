// Write every function with a non-default name as "address<TAB>name" (plus the namespace for
// class methods). Args: <out tsv>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class ExportNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(getScriptArgs()[0]);
        out.println("address\tname");
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            String name = f.getName(true);
            if (name.startsWith("FUN_") || name.startsWith("thunk_FUN_")) continue;
            out.println(String.format("%08x\t%s", f.getEntryPoint().getOffset(), name));
            n++;
        }
        out.close();
        println("names: " + n);
    }
}
