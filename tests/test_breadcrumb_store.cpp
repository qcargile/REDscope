#include <gtest/gtest.h>
#include "../src/breadcrumbs/BreadcrumbStore.h"
#include <cstring>
#include <vector>

TEST(BreadcrumbStore, CrumbPushesAndTruncates) {
    redscope::InitBreadcrumbStore();
    redscope::Crumb(redscope::BcUser, "TAG_TOO_LONG_TO_FIT_AND_THEN_SOME", "hello");

    std::vector<redscope::Breadcrumb> out;
    redscope::GetBreadcrumbStore().ring.Snapshot(
        [&](const redscope::Breadcrumb& b){ out.push_back(b); });

    ASSERT_GE(out.size(), 1u);
    const auto& b = out.back();
    EXPECT_STREQ(b.message, "hello");
    EXPECT_EQ(std::strlen(b.tag), redscope::kBreadcrumbTagLen - 1);
    EXPECT_EQ(b.kind, static_cast<uint32_t>(redscope::BcUser));
    EXPECT_GT(b.timestampNs, 0);
}
