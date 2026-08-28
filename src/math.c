
#include <system.h>
#include <time.h>
#include <math.h>

static int g_seed;

int pow(int num, int exp)
{
  int i=0;
  int number=num;

  for(i=1; i < exp -1; i++)
  {
    number = number * num;
  }
  
  return number;
}



int rand(void)
{
	if(g_seed == 0)
		g_seed = 1;
	if((((g_seed << 3) ^ g_seed) & 0x80000000uL) != 0)
		g_seed = (g_seed << 1) | 1;
	else
		g_seed <<= 1;
	return g_seed - 1;
}

/*
int rand()
{
  if(get_ticks() % 8 < 4)
  {
    lastrand = lastrand - get_ticks();
  } else {
    lastrand = lastrand + get_ticks();
  }
  
  if(lastrand < 0)
  {
    lastrand = lastrand * (-1);
  }
  
  return lastrand;
  
}
*/
