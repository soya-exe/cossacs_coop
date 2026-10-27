// Same as Decomp.java but with a 20 min decompiler timeout per function (for huge functions).
// Args: <out.txt> <addr|start-end>... ; a function address is decompiled, any address gets its xrefs listed.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

public class DecompLong extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface d = new DecompInterface();
        ghidra.app.decompiler.DecompileOptions o = new ghidra.app.decompiler.DecompileOptions();
        o.setMaxPayloadMBytes(400); o.setMaxInstructions(400000);
        d.setOptions(o);
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(args[0])) {
            java.util.List<Address> addrs = new java.util.ArrayList<>();
            for (int i = 1; i < args.length; i++) {
                if (args[i].contains("-")) { // range: every function whose entry is inside
                    String[] r = args[i].split("-");
                    for (Function f : currentProgram.getFunctionManager().getFunctions(
                            new ghidra.program.model.address.AddressSet(toAddr(r[0]), toAddr(r[1])), true))
                        addrs.add(f.getEntryPoint());
                } else addrs.add(toAddr(args[i]));
            }
            for (Address a : addrs) {
                w.println("=================================================================");
                w.println("## " + a);
                StringBuilder refs = new StringBuilder();
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    refs.append("  ").append(r.getFromAddress()).append(' ').append(r.getReferenceType())
                        .append(" in ").append(f == null ? "?" : f.getName()).append('\n');
                }
                w.print("xrefs:\n" + refs);
                Function f = getFunctionAt(a);
                if (f == null) continue;
                w.println("callers: " + f.getCallingFunctions(monitor));
                DecompileResults res = d.decompileFunction(f, 1200, monitor);
                w.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "<decompile failed> " + res.getErrorMessage());
            }
        }
    }
}
