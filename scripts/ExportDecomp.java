// Decompile every function and write C output grouped by address.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        new File(outDir).mkdirs();
        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        di.setOptions(opts);
        di.toggleCCode(true);
        di.openProgram(currentProgram);

        PrintWriter all = new PrintWriter(new FileWriter(outDir + "/perfect_all.c"));
        PrintWriter idx = new PrintWriter(new FileWriter(outDir + "/functions.tsv"));
        idx.println("address\tname\tsize\tstatus");
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0, fail = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            if (f.isThunk() || f.isExternal()) continue;
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            String status = "ok";
            all.println("// ==== " + f.getEntryPoint() + " " + f.getName() + " ====");
            if (r != null && r.decompileCompleted()) {
                all.println(r.getDecompiledFunction().getC());
            } else {
                status = "fail";
                fail++;
                all.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
            }
            idx.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + f.getBody().getNumAddresses() + "\t" + status);
            n++;
            if (n % 500 == 0) println("decompiled " + n);
        }
        all.close();
        idx.close();
        println("DONE functions=" + n + " failed=" + fail);
    }
}
