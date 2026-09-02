// Child class for the class-inheritance go-to-def demo.
// Extends pkt_base (pkt_base.sv). See class_inheritance_definition.sv for
// the fixture that exercises this.

class pkt_child extends pkt_base;
    int extra_field;

    // Go-to-def on `depth` here jumps to `pkt_base::depth` --
    // this is inherited field access from inside the child class body,
    // not external `obj.field` access.
    function void bump();
        depth = depth + 1;
    endfunction
endclass
