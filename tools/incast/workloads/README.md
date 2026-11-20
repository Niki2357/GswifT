# RPC size distributions for rpc_load (E8)

Each file: `<bytes> <cumulative probability>` per line; sizes are
interpolated linearly between points.

- `websearch.cdf`: the DCTCP web-search distribution (Alizadeh et al.,
  SIGCOMM 2010) as used by pFabric, HPCC and Homa (W5), truncated at 1 MB as
  in Homa/Linux (ATC 2021). Mean about 386 KB (Homa reports 385 KB for W5).
- `rpcmix.cdf`: a synthetic short-RPC-heavy mix (half under 1 KB, 3% above
  256 KB, mean about 37 KB). Not a published trace; it stands in for the
  small-message workloads Swift targets.
