#include "numeric.h"
#include <stdio.h>
#include <stdlib.h>]
#include <math.h>


char* numeric_int_to_string(int value)
{
	char* dat = memop_alloc_raw(22);

	int size = sprintf(dat, "%d", value);

	dat[size] = '\0';

	return dat;
}

//double numeric_parse_double(const char* data)
//{
//	double num = strtod(data, 0);
//	return num;
//}


double numeric_parse_double(char* data, int* check_error)
{
	int result      = 0;
	int is_negative = 0;
	int ix          = 0;
	int LENG        = 22;
	int is_decimal  = 0;
	int frac        = 0;
	int mult        = 0;

	if (data[0] == '-')
	{
		is_negative = 1;
		ix++;
	}

	while (ix < LENG)
	{
		if (data[ix] == 0) break;

		if (is_decimal)
		{
			mult++;

			if (data[ix] >= '0' && data[ix] <= '9')
			{
				frac = (frac * 10) + (data[ix] - '0');
			}
			else
			{
				if (check_error != 0) *check_error = 1;
				break;
			}
		}
		else
		{
			if (data[ix] == '.')
			{
				is_decimal = 1;
				ix++;
				continue;
			}

			if (data[ix] >= '0' && data[ix] <= '9')
			{
				result = (result * 10) + (data[ix] - '0');
			}
			else
			{
				if (check_error != 0) *check_error = 1;
				break;
			}
		}
		ix++;
	}

	double div = pow(10, mult);
	double res = result + (frac / div);

	if (is_negative)
	{
		res = -res;
	}

	return res;
}


int numeric_parse_int(char* data, int* check_error)
{
	int result = 0;
	int is_negative = 0;
	int ix = 0;
	int LENG = 22;

	if (data[0] == '-')
	{
		is_negative = 1;
		ix++;
	}

	while (ix < LENG)
	{
		if (data[ix] == 0) break;

		if (data[ix] >= '0' && data[ix] <= '9')
		{
			result = (result * 10) + (data[ix] - '0');
		}
		else
		{
			if (check_error != 0) *check_error = 1;
			break;
		}
		ix++;
	}

	if (is_negative)
	{
		result = -result;
	}

	return result;
}