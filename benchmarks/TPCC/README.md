# TPC-C — Order-Entry OLTP Benchmark

**Specification**: TPC Benchmark C, Standard Specification 5.11.
Transaction Processing Performance Council.

## Overview

TPC-C simulates a wholesale supplier with a number of warehouses.
It involves five concurrent transaction types:

| Transaction     | Read/Write | Description                            |
|----------------|------------|----------------------------------------|
| **NewOrder**   | Read-Write | Enter a complete order (backbone)      |
| **Payment**    | Read-Write | Update customer balance                |
| **OrderStatus**| Read-Only  | Check on order status                  |
| **Delivery**   | Read-Write | Process a batch of delivery            |
| **StockLevel** | Read-Only  | Monitor stock levels                   |

## Build

```sh
cd benchmarks/TPCC

make all                     # Build all backends
make tpcc_singlelock         # SingleGlobalLock
make tpcc_tl2                # TL2
make tpcc_tinystm            # TinySTM
make tpcc_persistentsgl      # PersistentSGL
make tpcc_uninstrumented     # Baseline (no TM)
```

## Usage

```sh
./bin/tpcc_<backend> -t <threads> -d <duration_ms>
```

## Official Resources

- **Specification**: https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf
- **Website**: https://www.tpc.org/tpcc/
