#define EOS '\0'

extern char *itoa(int val);
extern int atoi(char *val);
extern size_t strlen(const char *str);
extern char *	strcat(char *_s1, const char *_s2);
extern char *hextoa(int val);
//extern void strcpy(char *dest, char *source);
extern int atoi(char *val);
extern char *strsplit(char *str, int num, char delimiter);
extern int splitcount(char *str, char delimiter);
extern int strleft(char *str, char *search);
extern int strcharcount(char *str, char character);
extern char *replace(char *string, char *oldpiece, char *newpiece);
extern int prmc(char *str);
extern char *prmv(int num, char *str);
extern char *strcpy(char *dest, const char *src);
