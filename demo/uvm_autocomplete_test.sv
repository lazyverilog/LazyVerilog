// Manual UVM autocomplete fixture.
//
// This file is intentionally small and contains a few incomplete expressions so
// you can open it in Neovim and trigger completion at the marked locations.
// UVM should be discovered from demo/vcode.f, which lists the downloaded
// ./uvm-core/src/uvm_pkg.sv library file explicitly.  There is no special
// LazyVerilog UVM provider and +incdir+ is intentionally ignored.
//
// Suggested manual checks:
//   1. Put the cursor after `uvm_` in `extends uvm_|` below.
//      Expected: UVM classes/types discovered from uvm_pkg.
//      Expected keywords include: uvm_sequence_item, uvm_env, uvm_component,
//      uvm_object, uvm_phase, uvm_config_db.
//   2. Put the cursor after the backtick in `` `uvm_| `` below.
//      Expected: UVM macros discovered from uvm_macros.svh through uvm_pkg.sv.
//      Expected keywords include: uvm_object_utils, uvm_component_utils,
//      uvm_info, uvm_warning, uvm_error, uvm_fatal.
//   3. Put the cursor after `uvm_pkg::|` below.
//      Expected: package-scope UVM symbols.
//      Expected keywords include: uvm_object, uvm_component, uvm_sequence_item,
//      uvm_env, uvm_phase, uvm_config_db.
//   4. Put the cursor after `uvm_config_db#(uvm_object)::|` below.
//      Expected: class static methods if member/package type indexing can
//      resolve that symbol from the UVM library.
//      Expected keywords include: get, set, exists, wait_modified.

import uvm_pkg::*;

class lv_uvm_autocomplete_item extends uvm_sequence_item;
  rand bit [31:0] addr;
  rand bit [31:0] data;

  `uvm_object_utils(lv_uvm_autocomplete_item)

  function new(string name = "lv_uvm_autocomplete_item");
    super.new(name);
  endfunction
endclass

class lv_uvm_autocomplete_env extends uvm_env;
  `uvm_component_utils(lv_uvm_autocomplete_env)

  function new(string name = "lv_uvm_autocomplete_env", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);

    // Package-scope completion location:
    // Expected keywords: uvm_object, uvm_component, uvm_sequence_item, uvm_env,
    // uvm_phase, uvm_config_db.
    uvm_pkg::

    // Static member completion location:
    // Expected keywords: get, set, exists, wait_modified.
    uvm_config_db#(uvm_object)::
  endfunction
endclass

// Type-name completion location:
// Expected keywords: uvm_sequence_item, uvm_env, uvm_component, uvm_object,
// uvm_phase, uvm_config_db.
class lv_uvm_autocomplete_partial_extends extends uvm_
endclass

class lv_uvm_autocomplete_macro_area extends uvm_object;
  // Macro completion location:
  // Expected keywords: uvm_object_utils, uvm_component_utils, uvm_info,
  // uvm_warning, uvm_error, uvm_fatal.
  `uvm_
endclass
