#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

TM int x = 0;

TX void tx_func() {
    x = 1;
}

int main() { return 0; }
