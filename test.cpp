// test.cpp

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

struct S { int a; int b; };

__attribute__((annotate("tm")))
struct S s;

TM int x = 0;
TM short z = 0;
int y = 1;

TX void
tm_function2()
{
	x = y + 1;
	s.a = x;
}

void non_tm_function()
{
	x = y + 1;
	y = x;
	tm_function2();
}

TX void
tm_function()
{
	x = y + 1;
	s.a = x;
	z = 2;
	non_tm_function();
}

int main()
{

	tm_function();

	return 0;
}

