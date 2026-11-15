//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Regular Expression Support
//
// ⚠️ GalBox 重写（Milestone 2d）：本文件与 tjsRegExp.cpp 已**不是**上游原文。
//
// 上游这一层 832 行全部绑在 boost::reg_expression / boost::match_results 的
// 旧 C++ API 上（含 std::bind1st + std::mem_fun，C++11 起已废弃），且旧代码与
// 现代 boost 的编译差异本身就是一个坑。这里保留**同一个类名、同一套成员、
// 同一套语义**，只把后端换成标准库 std::basic_regex，接口形状与上游一致，
// 因此 tjs.cpp / tjsArray.cpp 的接入点一行都不用改。
//
// 上游原始 blob（sha 见 VENDOR_MANIFEST.json）随时可取回对比。
//---------------------------------------------------------------------------
#ifndef tjsRegExpH
#define tjsRegExpH

#include <regex>

#include "tjsNative.h"

namespace TJS
{
// tjs_char 在 Apple 平台是 4 字节 wchar_t（见 tjsTypes.h），直接给 std::regex 用。
typedef std::basic_regex<tjs_char> tTJSRegEx;
//---------------------------------------------------------------------------
// tTJSNI_RegExp
//---------------------------------------------------------------------------
class tTJSNI_RegExp : public tTJSNativeInstance
{
public:
	tTJSNI_RegExp();
	tTJSRegEx RegEx;
	// 上游用 boost 的 RegEx.empty() 判断「模式本身为空」。std::basic_regex 没有
	// 对应查询，改用编译时记录 —— 语义与 boost 一致：空模式在 Match 里被特殊
	// 处理（到达串尾即视为匹配成功）。
	bool EmptyPattern;
	tjs_uint32 Flags;
	tjs_uint Start;
	tTJSVariant Array;
	tjs_uint Index;
	ttstr Input;
	tjs_uint LastIndex;
	ttstr LastMatch;
	ttstr LastParen;
	ttstr LeftContext;
	ttstr RightContext;

public:
	void Split(iTJSDispatch2 ** array, const ttstr &target, bool purgeempty);
};
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTJSNC_RegExp
//---------------------------------------------------------------------------
class tTJSNC_RegExp : public tTJSNativeClass
{
public:
	tTJSNC_RegExp();

	static void Compile(tjs_int numparam, tTJSVariant **param, tTJSNI_RegExp *_this);
	static bool Match(std::match_results<const tjs_char *>& what,
		ttstr target, tTJSNI_RegExp *_this);
	static bool Exec(std::match_results<const tjs_char *>& what,
		ttstr target, tTJSNI_RegExp *_this);
	static iTJSDispatch2 * GetResultArray(bool matched, tTJSNI_RegExp *_this,
		const std::match_results<const tjs_char *>& what);

private:
	tTJSNativeInstance *CreateNativeInstance();

public:
	static tjs_uint32 ClassID;

	static tTJSVariant LastRegExp;
};
//---------------------------------------------------------------------------
extern iTJSDispatch2 * TJSCreateRegExpClass();
//---------------------------------------------------------------------------

} // namespace TJS

#endif
