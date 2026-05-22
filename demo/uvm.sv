function automatic uvm_reg_data_t get_csr_wdata_with_write_excl(
    uvm_reg csr,
    uvm_reg_data_t wdata,
    csr_test_type_e csr_test_type,
    csr_excl_item m_csr_excl_item=get_excl_item(csr)
);
    uvm_reg_field       flds                [$]             ;
    csr.get_fields(flds);

    foreach (flds[i]) begin
        if (m_csr_excl_item.is_excl(flds[i], CsrExclWrite, csr_test_type)) begin
            `uvm_info($sformatf("%m"),
                      $sformatf("Retain mirrored 0x%0h for field %0s due to CsrExclWrite exclusion", `gmv(flds[i]), flds[i].get_full_name()),
                      UVM_MEDIUM)
            wdata   = get_csr_val_with_updated_field(flds[i], wdata, `gmv(flds[i]));
        end
    end
    return wdata;
endfunction
