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
	int digits = 1;
	int i;
	int value;

	value = val;

	if(val >= 10){
		while(value >= 10)
		{
		  value = value / 10;
		  digits++;
		}
	}

	ret[digits] = EOS; /* end of the string (indices start at 0) */
	
	for(i = digits -1; i >= 0; i--)
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

int strcmp(const char *str1, const char *str2)
{
	/* Standard C semantics: 0 on equality, otherwise the difference of the
	   first differing character (compared as unsigned char).
	   This function used to return 1 for "equal" and read past the end of
	   str2 while doing so. */
	while(*str1 != EOS && *str1 == *str2)
	{
		str1++;
		str2++;
	}

	return (int)(unsigned char)*str1 - (int)(unsigned char)*str2;
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

	/* Terminating null directly after the appended text, not one position
	   beyond it. */
	_s1[lens1+lens2] = EOS;

	return _s1;
}

char *hextoa(int val)
{
	/* Prints the value itself (that is what printf("%X") in scrn.c expects)
	   and additionally returns the generated string. The buffer is static so
	   that the pointer stays valid after the return. */
	char hextoa_single(int);
	static char ret[20];
	char tmp[20];
	unsigned int uval;
	int i=0;
	int x=0;

	uval = (unsigned int)val;	/* %X prints unsigned */

	if(uval == 0)
	{
		/* Special case: the value 0 used to produce no output at all. */
		tmp[i] = '0';
		i++;
	}

	while(uval > 0)
	{
		tmp[i] = hextoa_single((int)(uval % 16));
		uval = uval / 16;
		i++;
	}

	/* tmp holds the digits in reverse order */
	while(i > 0)
	{
		i--;
		ret[x] = tmp[i];
		x++;
	}
	ret[x] = '\0';

	printf("%s", ret);
	return ret;
}

char hextoa_single(int val)
{
	if(val <= 9) return (char)('0' + val);	/* the 9 used to fall through */

	switch(val)
	{
		case 10: return 'A';
		case 11: return 'B';
		case 12: return 'C';
		case 13: return 'D';
		case 14: return 'E';
		case 15: return 'F';
	}

	return '?';	/* unreachable, but makes the function complete */
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
	/* Return buffer static as in itoa(): a pointer to a local array would be
	   invalid after the return. */
	static char ret[100];
	int i;
	int word_index=0;
	int out_pos=0;

	ret[0] = EOS;	/* definitely terminated, even if nothing is found */

	for(i=0; i < (int)strlen(str); i++)
	{
		if(str[i] == delimiter)
		{
			word_index++;
		} else {
			if(word_index == num && out_pos < (int)sizeof(ret)-1)
			{

				ret[out_pos] = str[i];
				out_pos++;
				ret[out_pos] = EOS;

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
	//returns the number of occurrences of a given
	//character in a string
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
	/* Return buffer static as in itoa(): a pointer to a local array would be
	   invalid after the return. */
	static char retstr[256];
	int i=0, j=0;
	int strs=0, stre=-1;

	retstr[0] = EOS;	/* definitely terminated */

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
		// a string was found!
		
		j=0;
		for(i=0; i <= strs-1 && j < (int)sizeof(retstr)-1; i++)
		{
			retstr[j] = string[j];
			j++;
		}
		for(i=0; i <= (int)strlen(newpiece)-1 && j < (int)sizeof(retstr)-1; i++)
		{
			retstr[j] = newpiece[i];
			j++;
		}
		for(i=stre+1; i <= (int)strlen(string) && j < (int)sizeof(retstr)-1; i++)
		{
			retstr[j] = string[i];
			j++;
		}

	}

	retstr[j] = EOS;	/* j is the write index, not i */
	return retstr;
}

char *prmv(int num, char *str)
{
	/* Return buffer static as in itoa(): a pointer to a local array would be
	   invalid after the return. */
	static char ret[100];
	int i;
	int word_index=0;
	int out_pos=0;

	/* Important: terminate immediately. Otherwise, for prmv(1, "taskmgr"),
	   the result of the previous call would still be in the buffer. */
	ret[0] = EOS;

	for(i=0; i < (int)strlen(str); i++)
	{
		if(str[i] == ' ')
		{
			word_index++;
		} else {
			if(word_index == num && out_pos < (int)sizeof(ret)-1)
			{

				ret[out_pos] = str[i];
				out_pos++;
				ret[out_pos] = EOS;

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
   while((*dest++ = *src++) != EOS);
   return save;
}
