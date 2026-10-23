/* SPDX-License-Identifier: GPL-3.0-only */

#include <stdbool.h>

#include <papermono/quotes.h>

/* Classical Chinese texts and poems whose authors died more than 70 years ago;
 * short excerpts with attribution. Keep each entry within two display lines at
 * 24 px so the standby band never needs reflow.
 */
static const struct papermono_quote quotes[] = {
	{ "学而时习之，不亦说乎。", "论语", "孔子" },
	{ "知之为知之，不知为不知，是知也。", "论语", "孔子" },
	{ "上善若水，水善利万物而不争。", "道德经", "老子" },
	{ "千里之行，始于足下。", "道德经", "老子" },
	{ "吾生也有涯，而知也无涯。", "庄子", "庄周" },
	{ "路漫漫其修远兮，吾将上下而求索。", "离骚", "屈原" },
	{ "长风破浪会有时，直挂云帆济沧海。", "行路难", "李白" },
	{ "会当凌绝顶，一览众山小。", "望岳", "杜甫" },
	{ "海内存知己，天涯若比邻。", "送杜少府之任蜀州", "王勃" },
	{ "采菊东篱下，悠然见南山。", "饮酒", "陶渊明" },
	{ "山重水复疑无路，柳暗花明又一村。", "游山西村", "陆游" },
	{ "不要人夸颜色好，只留清气满乾坤。", "墨梅", "王冕" },
	{ "无可奈何花落去，似曾相识燕归来。", "浣溪沙", "晏殊" },
	{ "竹杖芒鞋轻胜马，谁怕？一蓑烟雨任平生。", "定风波", "苏轼" },
};

size_t papermono_quote_count(void)
{
	return sizeof(quotes) / sizeof(quotes[0]);
}

const struct papermono_quote *papermono_quote_for_day(int year, int month,
	int day)
{
	static const uint16_t month_start[12] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
	};
	const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	int day_of_year;

	if (month < 1 || month > 12 || day < 1 || day > 31) {
		return &quotes[0];
	}
	day_of_year = month_start[month - 1] + day;
	if (leap && month > 2) {
		++day_of_year;
	}
	return &quotes[(size_t)(day_of_year - 1) % papermono_quote_count()];
}
