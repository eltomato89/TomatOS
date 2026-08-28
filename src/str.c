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

int strcmp(const char *str1, const char *str2)
{
	/* Uebliche C-Semantik: 0 bei Gleichheit, sonst die Differenz des
	   ersten abweichenden Zeichens (als unsigned char verglichen).
	   Frueher lieferte diese Funktion 1 fuer "gleich" und las dabei
	   ueber das Ende von str2 hinaus. */
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

	/* Abschlussnull direkt hinter den angehaengten Text, nicht eine
	   Position dahinter. */
	_s1[lens1+lens2] = EOS;

	return _s1;
}

char *hextoa(int val)
{
	/* Druckt den Wert selbst (so erwartet es printf("%X") in scrn.c) und
	   liefert zusaetzlich den erzeugten String zurueck. Der Puffer ist
	   statisch, damit der Zeiger nach dem Return gueltig bleibt. */
	char hextoa_single(int);
	static char ret[20];
	char tmp[20];
	unsigned int uval;
	int i=0;
	int x=0;

	uval = (unsigned int)val;	/* %X gibt vorzeichenlos aus */

	if(uval == 0)
	{
		/* Sonderfall: fuer den Wert 0 kam frueher gar nichts heraus. */
		tmp[i] = '0';
		i++;
	}

	while(uval > 0)
	{
		tmp[i] = hextoa_single((int)(uval % 16));
		uval = uval / 16;
		i++;
	}

	/* tmp enthaelt die Ziffern rueckwaerts */
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
	if(val <= 9) return (char)('0' + val);	/* die 9 fiel frueher durch */

	switch(val)
	{
		case 10: return 'A';
		case 11: return 'B';
		case 12: return 'C';
		case 13: return 'D';
		case 14: return 'E';
		case 15: return 'F';
	}

	return '?';	/* nicht erreichbar, macht die Funktion aber vollstaendig */
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
	/* Rueckgabepuffer statisch wie bei itoa(): ein Zeiger auf ein lokales
	   Array waere nach dem Return ungueltig. */
	static char ret[100];
	int i;
	int wnum=0;
	int cnum=0;

	ret[0] = EOS;	/* definiert terminiert, auch wenn nichts gefunden wird */

	for(i=0; i < (int)strlen(str); i++)
	{
		if(str[i] == delimiter)
		{
			wnum++;
		} else {
			if(wnum == num && cnum < (int)sizeof(ret)-1)
			{

				ret[cnum] = str[i];
				cnum++;
				ret[cnum] = EOS;

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
	/* Rueckgabepuffer statisch wie bei itoa(): ein Zeiger auf ein lokales
	   Array waere nach dem Return ungueltig. */
	static char retstr[256];
	int i=0, j=0;
	int strs=0, stre=-1;

	retstr[0] = EOS;	/* definiert terminiert */

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

	retstr[j] = EOS;	/* j ist der Schreibindex, nicht i */
	return retstr;
}

char *prmv(int num, char *str)
{
	/* Rueckgabepuffer statisch wie bei itoa(): ein Zeiger auf ein lokales
	   Array waere nach dem Return ungueltig. */
	static char ret[100];
	int i;
	int wnum=0;
	int cnum=0;

	/* Wichtig: sofort terminieren. Sonst stuende bei prmv(1, "taskmgr")
	   noch das Ergebnis des vorigen Aufrufs im Puffer. */
	ret[0] = EOS;

	for(i=0; i < (int)strlen(str); i++)
	{
		if(str[i] == ' ')
		{
			wnum++;
		} else {
			if(wnum == num && cnum < (int)sizeof(ret)-1)
			{

				ret[cnum] = str[i];
				cnum++;
				ret[cnum] = EOS;

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
