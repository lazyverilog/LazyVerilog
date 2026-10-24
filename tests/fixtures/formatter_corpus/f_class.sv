package f_pkg;
  class base #(type T = int, int N = 4) extends uvm_object implements if_a, if_b;
    rand T data[N];
    constraint c_sz { foreach (data[i]) data[i] inside {[0:10]}; solve data before x; }
    local static const int K = 3;
    extern virtual function void build(input int a = 1, output int b);
    function new(string name = "base"); super.new(name); endfunction
    virtual task run(); fork begin wait (x); end join_any disable fork; endtask
  endclass : base
  function void base::build(input int a = 1, output int b); b = a; endfunction
endpackage : f_pkg
