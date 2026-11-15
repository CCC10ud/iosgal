//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Regular Expression Support
//
// ⚠️ GalBox 重写（Milestone 2d）：**不是上游原文**。
//
// 上游实现绑 boost::reg_expression（regex_grep / match_results / std::bind1st），
// 本文件用标准库 std::basic_regex 重写，但保持与上游**完全一致的对外语义**：
//   - 类名 tTJSNC_RegExp / 实例 tTJSNI_RegExp / 工厂 TJSCreateRegExpClass()
//   - 成员：_compile / compile / test / match / exec / replace / split
//           start / index / lastIndex / input / lastMatch / lastParen /
//           leftContext / rightContext / matches / 静态 last
//   - 标志：g（全局，下一轮从上次结束处继续）/ i（忽略大小写）/ l（本地化排序，
//           std::regex 无对应，保持码点比较 —— 与上游 nocollate 一致）
//   - 正则字面量走 tjsInterCodeGen 的 `new RegExp() ; _compile("//flags/pat")`，
//     所以 _compile 必须能吃 "//flag/body" 这种内部表示。
//
// 与 boost 的已知差异（已在 test_m2d 里用判据覆盖）：
//   1) boost 的 "." 默认**匹配换行**，ECMAScript 默认不匹配 → 编译前把字符类
//      外的裸 '.' 翻译成 [\s\S]，还原上游行为；
//   2) boost 支持 \Q...\E，ECMAScript 不支持 → 未实现（KAG 脚本里未出现）；
//   3) replace 的替换串**不展开 $1**（上游就是这样，不是 JS 语义），保持一致。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "tjsRegExp.h"
#include "tjsArray.h"

#include <regex>
#include <string>

namespace TJS
{
//---------------------------------------------------------------------------
// Flags
//---------------------------------------------------------------------------
// some TJS flags
const tjs_uint32 globalsearch = (tjs_uint32)(((tjs_uint32)1)<<31);
const tjs_uint32 tjsflagsmask = (tjs_uint32)0xff000000;

//---------------------------------------------------------------------------
static tjs_uint32 TJSRegExpFlagToValue(tjs_char ch, tjs_uint32 prev)
{
	// converts flag letter to internal flag value.
	// this returns modified prev.
	// when ch is '\0', returns default flag value and prev is ignored.

	if(ch == 0)
	{
		// 上游是 regbase::normal|regbase::use_except|regbase::nocollate。
		// std::regex 出错必抛 std::regex_error（等于 use_except），
		// ECMAScript 语法最接近 TJS2/JS 的写法，字符范围按码点比较（等于 nocollate）。
		return (tjs_uint32)std::regex_constants::ECMAScript;
	}

	switch(ch)
	{
	case TJS_W('g'): // global search
		prev|=globalsearch; return prev;
	case TJS_W('i'): // ignore case
		prev|=(tjs_uint32)std::regex_constants::icase; return prev;
	case TJS_W('l'): // use localized collation (std::regex 无对应，保持码点比较)
		return prev;
	default:
		return prev;
	}
}
//---------------------------------------------------------------------------
static tjs_uint32 TJSGetRegExpFlagsFromString(const tjs_char *string)
{
	// returns a flag value represented by string

	tjs_uint32 flag = TJSRegExpFlagToValue(0, 0);

	while(*string && *string != TJS_W('/'))
	{
		flag = TJSRegExpFlagToValue(*string, flag);
		string++;
	}

	return flag;
}
//---------------------------------------------------------------------------
// boost 的 . 默认吃换行，ECMAScript 的 . 不吃 —— 语义差异会直接改变匹配结果，
// 所以编译前先做一次翻译：字符类（[...]）外、未被反斜杠转义的裸 '.' 换成
// [\s\S]。这样 /a.b/ 依然能匹配 "a\nb"，与上游一致。
static std::basic_string<tjs_char> TJSConvertDotToAnyChar(const std::basic_string<tjs_char> &pat)
{
	std::basic_string<tjs_char> out;
	out.reserve(pat.size() + 8);

	bool inclass = false;
	for(std::size_t i = 0; i < pat.size(); i++)
	{
		const tjs_char c = pat[i];

		if(c == TJS_W('\\'))
		{
			// 转义序列整体拷贝，\. 不能被翻译
			out += c;
			if(i + 1 < pat.size()) { out += pat[i + 1]; i++; }
			continue;
		}

		if(inclass)
		{
			out += c;
			if(c == TJS_W(']')) inclass = false;
			continue;
		}

		if(c == TJS_W('[')) { inclass = true; out += c; continue; }

		if(c == TJS_W('.')) { out += TJS_W("[\\s\\S]"); continue; }

		out += c;
	}

	return out;
}
//---------------------------------------------------------------------------
// std::regex_error 的正文是窄字符串，TJS_eTJSError 要宽字符串。
static void TJSThrowRegExpError(const std::exception &e)
{
	const char *what = e.what();
	std::basic_string<tjs_char> msg;
	while(what && *what) msg += (tjs_char)(unsigned char)*what++;
	if(msg.empty()) msg = TJS_W("regular expression error");
	TJS_eTJSError(msg.c_str());
}
//---------------------------------------------------------------------------
static void TJSAssignRegEx(tTJSNI_RegExp *_this,
	const std::basic_string<tjs_char> &expr, tjs_uint32 flags)
{
	const std::basic_string<tjs_char> translated = TJSConvertDotToAnyChar(expr);
	try
	{
		_this->RegEx.assign(translated.c_str(),
			(std::regex_constants::syntax_option_type)(flags & ~tjsflagsmask));
	}
	catch(std::exception &e)
	{
		TJSThrowRegExpError(e);
	}

	_this->Flags = flags;
	_this->EmptyPattern = translated.empty();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// replace 的迭代体（对应上游 tTJSReplacePredicator）
//
// 上游用 boost::regex_grep + 回调：回调返回 true 继续找下一个（由 g 标志决定），
// 返回 false 结束。这里换成显式循环，但结果拼装顺序与上游逐行对齐：
//   每命中一次 → 先补「上一处结束到本处开始」的原文 → 再补替换串 → lastpos 前移。
// 替换串**原样写入**，不展开 $1/$2（上游即如此）。
// 替换串若为函数，则用「本次匹配的结果数组」调用它，把返回值转成字符串写入。
//---------------------------------------------------------------------------
static void TJSDoReplace(iTJSDispatch2 *objthis, tTJSVariant **param,
	tjs_int numparams, tTJSNI_RegExp *_this, ttstr &res)
{
	const ttstr target = *param[0];

	ttstr to;
	tTJSVariantClosure funcval;
	bool func = false;

	if(param[1]->Type() != tvtObject)
	{
		to = (*param[1]);
	}
	else
	{
		funcval = param[1]->AsObjectClosureNoAddRef();
		if(funcval.ObjThis == NULL)
		{
			// replace with objthis when funcval's objthis is null
			funcval.ObjThis = objthis;
		}
		func = true;
	}

	const tjs_char *s = target.c_str();
	const tjs_int targlen = target.GetLen();
	const tjs_char *end = s + targlen;
	const tjs_char *p = s;

	tjs_int lastpos = 0;
	// 零宽匹配（如 /(?:)/g 或 /x*/g）在 global 下会原地反复命中，
	// 必须有硬上限，否则就是一个死循环（regex_grep 内部会前进一个字符）。
	tjs_int guard = targlen + 2;

	while(p <= end && guard-- > 0)
	{
		std::match_results<const tjs_char *> what;
		if(!std::regex_search(p, end, what, _this->RegEx)) break;

		const tjs_int pos = (tjs_int)(p - s) + (tjs_int)what.position();
		const tjs_int len = (tjs_int)what.length();

		if(pos > lastpos)
			res += ttstr(s + lastpos, pos - lastpos);

		if(!func)
		{
			res += to;
		}
		else
		{
			// call the callback function described as param[1]
			tTJSVariant result;
			iTJSDispatch2 *array =
				tTJSNC_RegExp::GetResultArray(true, _this, what);
			tTJSVariant arrayval(array, array);
			tTJSVariant *ap = &arrayval;
			array->Release();
			const tjs_error hr =
				funcval.FuncCall(0, NULL, NULL, &result, 1, &ap, objthis);
			// 上游的回调在这里 `return hr;`（bool 化后中途停掉，替换串悄悄丢失）。
			// 静默丢字符比抛错更难查，这里改成把错误抛回脚本。
			if(TJS_FAILED(hr))
				TJS_eTJSError(TJS_W("RegExp.replace: replacement function failed"));
			result.ToString();
			res += ttstr(result);
		}

		lastpos = pos + len;

		if(!(_this->Flags & globalsearch))
			break;   // 非 global：只替换第一处（上游靠回调返回 false 实现）

		if(len == 0)
		{
			// 零宽匹配：只把「搜索起点」往前挪一格，**不能**动 lastpos ——
			// lastpos 决定原文片段从哪里续接，挪了就会把这一个字符吃掉。
			if(lastpos >= targlen) break;
			p = s + lastpos + 1;
		}
		else
		{
			p = s + lastpos;
		}
	}

	if(lastpos < targlen)
		res += ttstr(s + lastpos, targlen - lastpos);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNI_RegExp : TJS Native Instance : RegExp
//---------------------------------------------------------------------------
tTJSNI_RegExp::tTJSNI_RegExp()
{
	// C++constructor
	Flags = TJSRegExpFlagToValue(0, 0);
	EmptyPattern = true;
	Start = 0;
	Index = 0;
	LastIndex = 0;

	// 必须给 RegEx 一个**合法的空模式**：boost 的默认 reg_expression 是
	// 「空但有效」的对象，而 libc++ 的 basic_regex 默认构造后再 regex_search
	// 属于未定义行为（内部指针为空）。脚本里 `var r = new RegExp();`
	// 之后直接 test/match 是合法写法，所以这里补一次 (?:) 编译。
	try
	{
		RegEx.assign(TJS_W("(?:)"),
			(std::regex_constants::syntax_option_type)(Flags & ~tjsflagsmask));
	}
	catch(...)
	{
		// (?:) 在 ECMAScript 语法下不可能失败；万一失败也不该在构造期抛。
	}
}
//---------------------------------------------------------------------------
void tTJSNI_RegExp::Split(iTJSDispatch2 ** array, const ttstr &target, bool purgeempty)
{
	// 与上游 tTJSSplitPredicator 逐条对齐（含 purgeempty 与子表达式展开规则）。
	bool arrayallocated = false;
	if(!*array) { *array = TJSCreateArrayObject(), arrayallocated = true; }

	try
	{
		const tjs_char *s = target.c_str();
		const tjs_int targlen = target.GetLen();
		const tjs_char *end = s + targlen;
		const tjs_char *p = s;

		tjs_int lastpos = 0;
		tjs_int lastlen = 0;
		tjs_int storecount = 0;
		tjs_int guard = targlen + 2;

		while(p <= end && guard-- > 0)
		{
			std::match_results<const tjs_char *> what;
			if(!std::regex_search(p, end, what, RegEx)) break;

			const tjs_int pos = (tjs_int)(p - s) + (tjs_int)what.position();
			const tjs_int len = (tjs_int)what.length();

			if(pos >= lastpos && (len || pos != 0))
			{
				if(!purgeempty || pos - lastpos)
				{
					tTJSVariant val(ttstr(s + lastpos, pos - lastpos));
					(*array)->PropSetByNum(TJS_MEMBERENSURE, storecount++, &val, *array);
				}
			}

			if(what.size() > 1)
			{
				// output sub-expression
				for(std::size_t i = 1; i < what.size(); i++)
				{
					const tjs_int sublen = what[i].matched ?
						(tjs_int)(what[i].second - what[i].first) : 0;
					if(!purgeempty || sublen)
					{
						tTJSVariant val;
						if(sublen)
							val = ttstr(what[i].first, sublen);
						else
							val = tTJSVariant(TJS_W(""));
						(*array)->PropSetByNum(TJS_MEMBERENSURE, storecount++, &val, *array);
					}
				}
			}

			lastpos = pos + len;
			lastlen = len;

			if(len == 0)
			{
				// 零宽匹配：只推进搜索起点，lastpos 保持不动
				// （它决定下一段原文从哪里切，动了就会吃掉一个字符）。
				if(lastpos >= targlen) break;
				p = s + lastpos + 1;
				lastlen = 0;
			}
			else
			{
				p = s + lastpos;
			}
		}

		// output last
		if(lastlen != 0 || lastpos != targlen)
		{
			// unless null match at last of target
			if(!purgeempty || targlen - lastpos)
			{
				tTJSVariant val(ttstr(s + lastpos, targlen - lastpos));
				(*array)->PropSetByNum(TJS_MEMBERENSURE, storecount++, &val, *array);
			}
		}
	}
	catch(...)
	{
		if(arrayallocated) (*array)->Release();
		throw;
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNC_RegExp : TJS Native Class : RegExp
//---------------------------------------------------------------------------
tTJSVariant tTJSNC_RegExp::LastRegExp;
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_RegExp::ClassID = (tjs_uint32)-1;
tTJSNC_RegExp::tTJSNC_RegExp() :
	tTJSNativeClass(TJS_W("RegExp"))
{
	// class constructor

	TJS_BEGIN_NATIVE_MEMBERS(/*TJS class name*/RegExp)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var. name*/_this, /*var. type*/tTJSNI_RegExp,
	/*TJS class name*/ RegExp)
{
	/*
		TJS constructor
	*/

	if(numparams >= 1)
	{
		tTJSNC_RegExp::Compile(numparams, param, _this);
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/RegExp)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/compile)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);

	/*
		compiles given regular expression and flags.
	*/

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSNC_RegExp::Compile(numparams, param, _this);

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/compile)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/_compile)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);

	/*
		internal function; compiles given constant regular expression.
		input expression is following format:
		//flags/expression
		where flags is flag letters ( [gil] )
		and expression is a Regular Expression

		这个成员是正则字面量的唯一入口：tjsInterCodeGen.cpp 对 T_REGEXP 生成
		`new RegExp(); _compile("//flag/body")`。删掉它，脚本里每一个 /.../ 都会
		在运行期变成 "Member _compile does not exist"。
	*/

	if(numparams != 1) return TJS_E_BADPARAMCOUNT;

	ttstr expr = *param[0];

	const tjs_char *p = expr.c_str();
	if(!p || !p[0]) return TJS_E_FAIL;

	if(p[0] != TJS_W('/') || p[1] != TJS_W('/')) return TJS_E_FAIL;

	p+=2;
	const tjs_char *exprstart = TJS_strchr(p, TJS_W('/'));
	if(!exprstart) return TJS_E_FAIL;
	exprstart ++;

	const tjs_uint32 flags = TJSGetRegExpFlagsFromString(p);

	try
	{
		TJSAssignRegEx(_this, std::basic_string<tjs_char>(exprstart), flags);
	}
	catch(std::exception &e)
	{
		TJSThrowRegExpError(e);
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/_compile)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/test)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);

	/*
		do the text searching.
		return match found ( true ), or not found ( false ).
		this function *changes* internal status.
	*/

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr target(*param[0]);
	std::match_results<const tjs_char *> what;
	const bool matched = tTJSNC_RegExp::Exec(what, target, _this);

	tTJSNC_RegExp::LastRegExp = tTJSVariant(objthis, objthis);

	if(result)
	{
		*result = matched;
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/test)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/match)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);

	/*
		do the text searching.
		this function is the same as test, except for its return value.
		match returns an array that contains each matching part.
		if match failed, returns empty array. eg.
		any internal status will not be changed.
	*/

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;


	if(result)
	{
		ttstr target(*param[0]);
		std::match_results<const tjs_char *> what;
		const bool matched = tTJSNC_RegExp::Match(what, target, _this);
		iTJSDispatch2 *array = tTJSNC_RegExp::GetResultArray(matched, _this, what);
		*result = tTJSVariant(array, array);
		array->Release();
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/match)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/exec)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);

	/*
		same as the match except for the internal status' change.
		var ar;
		var pat = /:(\d+):(\d+):/g;
		while((ar = pat.match(target)).count)
		{
			// ...
		}
	*/

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr target(*param[0]);
	std::match_results<const tjs_char *> what;
	tTJSNC_RegExp::Exec(what, target, _this);

	tTJSNC_RegExp::LastRegExp = tTJSVariant(objthis, objthis);

	if(result)
	{
		*result = _this->Array;
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/exec)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/replace)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);

	/*
		replaces the string

		newstring = /regexp/.replace(orgstring, newsubstring);
		newsubstring can be:
			1. normal string ( literal or expression that respresents string )
			2. a function
		function is called as in RegExp's context, returns new substring.

		or

		newstring = string.replace(/regexp/, newsubstring);
			( via String.replace method )

		replace method ignores start property, and does not change any
			internal status.
	*/

	if(numparams < 2) return TJS_E_BADPARAMCOUNT;

	ttstr res;
	TJSDoReplace(objthis, param, numparams, _this, res);

	if(result) *result = res;

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/replace)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/split)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);

	/*
		replaces the string

		array = /regexp/.replace(targetstring, <reserved>, purgeempty);

		or

		array = targetstring.split(/regexp/, <reserved>, purgeempty);

		or

		array = [].split(/regexp/, targetstring, <reserved>, purgeempty);

		this method does not update properties
	*/

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr target(*param[0]);

	bool purgeempty = false;
	if(numparams >= 3) purgeempty = param[2]->operator bool();

	iTJSDispatch2 *array = NULL;

	_this->Split(&array, target, purgeempty);

	if(result) *result = tTJSVariant(array, array);

	array->Release();

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/split)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(matches)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = _this->Array;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(matches)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(start)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = (tTVInteger)_this->Start;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/* var. name */_this, /* var. type */tTJSNI_RegExp);
		_this->Start = (tjs_uint)(tTVInteger)*param;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(start)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(index)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = (tTVInteger)_this->Index;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(index)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(lastIndex)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = (tTVInteger)_this->LastIndex;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(lastIndex)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(input)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = _this->Input;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(input)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(lastMatch)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = _this->LastMatch;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(lastMatch)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(lastParen)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = _this->LastParen;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(lastParen)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(leftContext)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = _this->LeftContext;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(leftContext)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(rightContext)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_RegExp);
		*result = _this->RightContext;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(rightContext)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(last)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = tTJSNC_RegExp::LastRegExp;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(last)
//---------------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_RegExp::CreateNativeInstance()
{
	return new tTJSNI_RegExp();
}
//---------------------------------------------------------------------------
void tTJSNC_RegExp::Compile(tjs_int numparams, tTJSVariant **param, tTJSNI_RegExp *_this)
{
	ttstr expr = *param[0];

	tjs_uint32 flags;
	if(numparams >= 2)
	{
		ttstr fs = *param[1];
		flags = TJSGetRegExpFlagsFromString(fs.c_str());
	}
	else
	{
		flags = TJSRegExpFlagToValue(0, 0);
	}

	// 官方：空模式替换成 (?:) —— 让「空正则」仍然是一次合法的、能匹配空串的编译。
	std::basic_string<tjs_char> pattern = expr.c_str();
	if(pattern.empty()) pattern = TJS_W("(?:)");

	try
	{
		TJSAssignRegEx(_this, pattern, flags);
	}
	catch(std::exception &e)
	{
		TJSThrowRegExpError(e);
	}
}
//---------------------------------------------------------------------------
bool tTJSNC_RegExp::Match(std::match_results<const tjs_char *>& what,
		ttstr target, tTJSNI_RegExp *_this)
{
	const tjs_uint targlen = target.GetLen();

	if(_this->Start == targlen)
	{
		// Start already reached at end
		return _this->EmptyPattern;  // returns true if empty
	}
	else if(_this->Start > targlen)
	{
		// Start exceeds target's length
		return false;
	}

	const tjs_char *begin = target.c_str() + _this->Start;
	const tjs_char *end = target.c_str() + targlen;
	return std::regex_search(begin, end, what, _this->RegEx);
}
//---------------------------------------------------------------------------
bool tTJSNC_RegExp::Exec(std::match_results<const tjs_char *>& what,
		ttstr target, tTJSNI_RegExp *_this)
{
	const bool matched = tTJSNC_RegExp::Match(what, target, _this);
	iTJSDispatch2 *array = tTJSNC_RegExp::GetResultArray(matched, _this, what);

	_this->Array = tTJSVariant(array, array);
	array->Release();

	_this->Input = target;
	if(!matched || _this->EmptyPattern)
	{
		_this->Index = _this->Start;
		_this->LastIndex = _this->Start;
		_this->LastMatch = ttstr();
		_this->LastParen = ttstr();
		_this->LeftContext = ttstr(target, _this->Start);
	}
	else
	{
		_this->Index = _this->Start + (tjs_uint)what.position();
		_this->LastIndex = _this->Start + (tjs_uint)what.position()
			+ (tjs_uint)what.length();
		_this->LastMatch = ttstr(what[0].first,
			(tjs_int)(what[0].second - what[0].first));
		const std::size_t last = what.size() - 1;
		_this->LastParen = what[last].matched ?
			ttstr(what[last].first, (tjs_int)(what[last].second - what[last].first)) :
			ttstr();
		_this->LeftContext = ttstr(target, _this->Start + (tjs_int)what.position());
		_this->RightContext = ttstr(target.c_str() + _this->LastIndex);
		if(_this->Flags & globalsearch)
		{
			// global search flag changes the next search starting position.
			_this->Start = _this->LastIndex;
		}
	}

	return matched;
}
//---------------------------------------------------------------------------
iTJSDispatch2 * tTJSNC_RegExp::GetResultArray(bool matched, tTJSNI_RegExp *_this,
		const std::match_results<const tjs_char *>& what)
{
	iTJSDispatch2 *array = TJSCreateArrayObject();
	if(matched)
	{
		if(_this->EmptyPattern)
		{
			tTJSVariant val(TJS_W(""));
			array->PropSetByNum(TJS_MEMBERENSURE|TJS_IGNOREPROP,
					0, &val, array);
		}
		else
		{
			const std::size_t size = what.size();
			try
			{
				for(std::size_t i=0; i<size; i++)
				{
					tTJSVariant val;
					if(what[i].matched)
						val = ttstr(what[i].first,
							(tjs_int)(what[i].second - what[i].first));
					else
						val = tTJSVariant(TJS_W(""));
					array->PropSetByNum(TJS_MEMBERENSURE|TJS_IGNOREPROP,
						(tjs_int)i, &val, array);
				}
			}
			catch(...)
			{
				array->Release();
				throw;
			}
		}
	}
	return array;
}
//---------------------------------------------------------------------------
iTJSDispatch2 * TJSCreateRegExpClass()
{
	return new tTJSNC_RegExp();
}
//---------------------------------------------------------------------------

} // namespace TJS
