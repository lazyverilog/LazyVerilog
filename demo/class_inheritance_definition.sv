// Parent/child class fixture for go-to-definition testing.
// pkt_base (pkt_base.sv) and pkt_child (pkt_child.sv) are separate project
// files, listed in vcode.f as extra files.
//
// Open this file in Neovim and go-to-def on `depth` and `apply` in
// `child.depth` / `child.apply()` below. Both are declared only on the
// parent `pkt_base`, not on `pkt_child` itself.
//
// Go-to-def walks the `extends` chain, so both jumps land on the declarations
// in `pkt_base`.

module class_inheritance_definition_demo;
    pkt_child child;

    initial begin
        child.depth = 1;
        child.apply();
    end
endmodule
