/* Mozilla's root certificates (PEM), the default trust store for TLS. */
    .section .rodata
    .global _ca_bundle, _ca_bundle_end
_ca_bundle:
    .incbin "assets/certs/ca-bundle.pem"
_ca_bundle_end:
    .byte 0

    .section .note.GNU-stack, "", @progbits
