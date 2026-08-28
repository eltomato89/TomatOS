/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: str.c string handling
*
*  Notes: No warranty expressed or implied. Use at own risk. */

#include <system.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define EOS '\0'
#define NULL 0

char *itoa(int val)
{
	char itoa_single(int);
	static char ret[50];
	int stellen = 1;
	int i;
	int x=0;
	int zahl;

	zahl = val;

	if(val >= 10){
		while(zahl >= 10)
		{
		  zahl = zahl / 10;
		  stellen++;
		}
	}

	ret[stellen] = EOS; /* Ende des Strings (bei 0 beginnen) */
	
	for(i = stellen -1; i >= 0; i--)
	{
		ret[i] = itoa_single((int)(val % 10));
		val = val / 10;
	}
	

  return ret;
  
}

char itoa_single(int val)
{
	return val + '0';
}

int strcmp(char *str1, char *str2)
{
  int i;
  int laenge_str1;
  
  laenge_str1 = strlen(str1);
  
	for(i = 0; i <= laenge_str1; i++)
	{
		if(str1[i] != str2[i])
    {
      return 0;
    }
  }
  return 1;
}

size_t strlen(const char *str)
{
    size_t retval;
    for(retval = 0; *str != EOS; str++) retval++;
    return retval;
}

char *	strcat(char *_s1, const char *_s2)
{
	
	int i;
	int lens1, lens2;

	lens1 = (int)strlen(_s1);
	lens2 = (int)strlen(_s2);


	for(i = 0; i < lens2; i++)
	{
		_s1[lens1+i] = _s2[i];
	}
	
	_s1[(int)strlen(_s1)+1] = EOS;

	return 0;
}

char *hextoa(int val)
{
	char hextoa_single(int);
	int rest;
	int i=-1;
	int x=0;
	char tmp[20];
	char ret[20];
	unsigned int neg=0;
	if(val < 0) neg=1;
	
	while(val > 0)
	{
		i++;
		rest = val % 16;
		val = val / 16;
		tmp[i] = hextoa_single(rest);
		if(val < 16)
		{
			i++;
			tmp[i] = hextoa_single(val);
			tmp[i+1] = '\0';
			break;
		}	
		
	}

	for(;i >= 0; i--)
	{
		ret[x] = tmp[i];
		x++;
	}
	ret[x] = '\0';
	//if(neg=1) printf("-");
	printf("%s", ret);
	return 0;
}

char hextoa_single(int val)
{
	if(val < 9) return '0' + val;
	
	switch(val)
	{
		case 10: return 'A';
		case 11: return 'B';
		case 12: return 'C';
		case 13: return 'D';
		case 14: return 'E';
		case 15: return 'F';
	}
}

int atoi(char* string)
{
   int i = 0;
   int value = 0;
   int zeroValue = '0';       
   const int slen = strlen(string);
   for (i = 0; i < slen; ++i)
   {
      char c = string[i];
      if (c < '0' || c > '9')
      {
         return -1;
      }
      value = 10 * value + (c - zeroValue);
   }
    return value;
}

char *strsplit(char *str, int num, char delimiter)
{
	int i;
	int wnum=0;
	int cnum=0;
	char ret[100];

	for(i=0; i <= (int)strlen(str); i++)
	{
		if(str[i] == delimiter)
		{
			wnum++;
		} else {
			if(wnum == num)
			{

				ret[cnum] = str[i];
				cnum++;
				ret[cnum] = '\0';
				
			}
		}
	}
	
	return ret;
}

int splitcount(char *str, char delimiter)
{
	int i;
	int c=0;
	for(i=0; i < (int)strlen(str); i++)
	{
		if(str[i] == delimiter) c++;
	}
	return c;
}

int strleft(char *str, char *search)
{
	int i;
	for(i=0; i < (int)strlen(search); i++)
	{
		if(str[i] != search[i]) return -1;
	}
	return 1;
}

int strcharcount(char *str, char character)
{	
	//gibt die Anzahl bestimmter Buchstaben
	//in einem String zurück
	int i;
	int cnt=0;
	for(i=0; i <= strlen(str); i++)
	{
		if(str[i] == character) cnt++;
	}
	return cnt;
}


char *replace(char *string, char *oldpiece, char *newpiece)
{
	char retstr[256];
	int i=0, j=0;
	int strs, stre;

	for(i=0; i <= (int)strlen(string)-1; i++)
	{
		if(string[i] == oldpiece[j])
		{
			strs = i;
			for(j=0; j <= (int)strlen(oldpiece)-1; j++)
			{
				if(string[i] == oldpiece[j])
				{
					stre = i;
				} else {
					j = -1;
					break;
				}
				i++;
			}

		}
	}
	if(j != -1)
	{
		// es wurde ein string gefunden!
		
		j=0;
		for(i=0; i <= strs-1; i++)
		{
			retstr[j] = string[j];
			j++;
		}
		for(i=0; i <= (int)strlen(newpiece)-1; i++)
		{
			retstr[j] = newpiece[i];
			j++;
		}
		i = stre+1;
		for(i=stre+1; i <= (int)strlen(string); i++)
		{
			retstr[j] = string[i];
			j++;
		}

	}
	
	retstr[i] = '\0';
	return retstr;
} 

char *prmv(int num, char *str)
{
	int i;
	int wnum=0;
	int cnum=0;
	char ret[100];

	for(i=0; i <= (int)strlen(str); i++)
	{
		if(str[i] == ' ')
		{
			wnum++;
		} else {
			if(wnum == num)
			{

				ret[cnum] = str[i];
				cnum++;
				ret[cnum] = '\0';
				
			}
		}
	}
	
	return ret;
}

int prmc(char *str)
{
	int i;
	int c=0;
	for(i=0; i < (int)strlen(str); i++)
	{
		if(str[i] == ' ') c++;
	}
	return c;
}

char *strcpy(char *dest, const char *src)
{
   char *save = dest;
   while(*dest++ = *src++);
   return save;
}
