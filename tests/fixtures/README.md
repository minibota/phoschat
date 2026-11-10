# IRC loopback TLS fixtures

The PEM files belong exclusively to the deterministic IRC tests. All sockets connect to `127.0.0.1` without DNS. The valid server certificate contains a `127.0.0.1` IP subject alternative name. The mismatch fixture contains `127.0.0.2` and is served on the same `127.0.0.1` endpoint. Both leaves are signed by the dedicated test CA and share the public fixture server key. The CA signing key is absent from the repository.

Tests inject trust only into their client instances and exercise both untrusted issuer and incorrect certificate identity with peer verification enabled. Replace the fixtures before 2044.
