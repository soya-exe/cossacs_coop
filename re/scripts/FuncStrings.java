// For each given function address, prints the strings referenced by it and by its direct callees,
// plus its callers. Used to classify which subsystem a function belongs to.
// Args: <out.txt> <addr>...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.util.*;

public class FuncStrings extends GhidraScript {
    void strings(Function f, Set<String> out) {
        for (Address a : f.getBody().getAddresses(true)) {
            for (Reference r : getReferencesFrom(a)) {
                Data d = getDataAt(r.getToAddress());
                if (d != null && d.hasStringValue()) {
                    out.add(String.valueOf(d.getValue()));
                }
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Function f = getFunctionContaining(toAddr(args[i]));
                if (f == null) { w.println("## " + args[i] + " no function"); continue; }
                Set<String> own = new LinkedHashSet<>(), sub = new LinkedHashSet<>();
                strings(f, own);
                for (Function c : f.getCalledFunctions(monitor)) strings(c, sub);
                sub.removeAll(own);
                w.println("## " + f.getName() + " @" + f.getEntryPoint());
                w.println("  callers: " + f.getCallingFunctions(monitor));
                w.println("  own strings: " + own);
                w.println("  callee strings: " + sub);
            }
        }
    }
}
