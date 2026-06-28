#include <gtest/gtest.h>
#include "../src/breadcrumbs/CrashArchive.h"
#include <string>
#include <vector>

using redscope::api::BuildCrashArray;
using redscope::api::ProjectCrashSummary;

TEST(CrashArchive, BuildArrayEmpty) {
    EXPECT_EQ(BuildCrashArray({}), "[]");
}

TEST(CrashArchive, BuildArrayOne) {
    EXPECT_EQ(BuildCrashArray({"{\"a\":1}"}), "[{\"a\":1}]");
}

TEST(CrashArchive, BuildArrayMultiple) {
    EXPECT_EQ(BuildCrashArray({"{\"a\":1}", "{\"b\":2}"}), "[{\"a\":1},{\"b\":2}]");
}

TEST(CrashArchive, BuildArraySkipsEmptyAndWhitespace) {
    EXPECT_EQ(BuildCrashArray({"{\"a\":1}", "", "  \n", "{\"b\":2}"}), "[{\"a\":1},{\"b\":2}]");
}

TEST(CrashArchive, ProjectExtractsGroupingFieldsAndDropsInstalledMods) {
    std::string full = R"({"schema":2,"tool":"REDscope","crashTimeUnix":1700000000,"crashId":"QAJYF537","looseId":"LOOSE001","fingerprint":{"primary":"1","loose":"2","modSet":"3","faultSiteUnreliable":true,"hasModule":true},"exception":{"code":"0xC0000005","address":"0x0"},"faultingModule":{"name":"REDscope.dll","rva":"0x40"},"stackModules":[{"name":"REDscope.dll","hits":2,"kind":"mod"},{"name":"ntdll.dll","hits":1,"kind":"system"}],"buildId":"3.0","crashClass":"native","installedMods":[{"name":"BigMod","version":"1.0","enabled":true}]})";
    std::string proj = ProjectCrashSummary(full);
    EXPECT_NE(proj.find("\"crashId\":\"QAJYF537\""), std::string::npos);
    EXPECT_NE(proj.find("\"looseId\":\"LOOSE001\""), std::string::npos);
    EXPECT_NE(proj.find("\"crashTimeUnix\":1700000000"), std::string::npos);
    EXPECT_NE(proj.find("\"crashClass\":\"native\""), std::string::npos);
    EXPECT_NE(proj.find("\"faultingModule\":\"REDscope.dll\""), std::string::npos);
    EXPECT_NE(proj.find("\"faultSiteUnreliable\":true"), std::string::npos);
    EXPECT_NE(proj.find("\"name\":\"ntdll.dll\""), std::string::npos);
    EXPECT_NE(proj.find("\"hits\":2"), std::string::npos);
    EXPECT_EQ(proj.find("installedMods"), std::string::npos);
    EXPECT_EQ(proj.find("BigMod"), std::string::npos);
}

TEST(CrashArchive, ProjectNullFaultingModuleAndEmptyStack) {
    std::string full = R"({"crashId":"A","looseId":"B","crashTimeUnix":1,"faultingModule":null,"stackModules":[],"crashClass":"Unknown"})";
    std::string proj = ProjectCrashSummary(full);
    EXPECT_NE(proj.find("\"faultingModule\":null"), std::string::npos);
    EXPECT_NE(proj.find("\"stackModules\":[]"), std::string::npos);
    EXPECT_NE(proj.find("\"crashId\":\"A\""), std::string::npos);
}

TEST(CrashArchive, ProjectReEscapesSpecialChars) {
    std::string full = R"({"crashId":"A","looseId":"B","crashTimeUnix":1,"faultingModule":{"name":"Wei\"rd\\Mod.dll"},"stackModules":[],"crashClass":"x"})";
    std::string proj = ProjectCrashSummary(full);
    EXPECT_NE(proj.find("Wei\\\"rd\\\\Mod.dll"), std::string::npos);
}

TEST(CrashArchive, ProjectMalformedOrEmptyReturnsEmpty) {
    EXPECT_EQ(ProjectCrashSummary("not json {{{"), "");
    EXPECT_EQ(ProjectCrashSummary(""), "");
}
