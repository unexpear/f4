#include <doctest/doctest.h>

#include "approx.hpp"
#include "core/math/mat4.h"
#include "core/math/transform.h"

using namespace f4::math;
using namespace f4test;

namespace {

const Vec3 xAxis(1.0f, 0.0f, 0.0f);
const Vec3 yAxis(0.0f, 1.0f, 0.0f);

} // namespace

TEST_CASE("identity leaves points and matrices unchanged") {
    const Mat4 i = Mat4::identity();
    const Vec3 p(1.5f, -2.0f, 3.25f);
    CHECK(veq(i.transformPoint(p), p));
    CHECK(meq(i.mul(Mat4::translation(Vec3(1.0f, 2.0f, 3.0f))), Mat4::translation(Vec3(1.0f, 2.0f, 3.0f))));
}

TEST_CASE("column major storage convention") {
    const Mat4 t = Mat4::translation(Vec3(7.0f, 8.0f, 9.0f));
    CHECK(t(0, 3) == 7.0f);
    CHECK(t(1, 3) == 8.0f);
    CHECK(t(2, 3) == 9.0f);
    CHECK(t.col[3].x == 7.0f);
    CHECK(t.col[3].w == 1.0f);
}

TEST_CASE("rotation about Z by 90 degrees maps axes") {
    const Mat4 r = Mat4::rotationZ(radians(90.0f));
    CHECK(veq(r.transformVector(xAxis), yAxis));
    CHECK(veq(r.transformVector(yAxis), -xAxis));
}

TEST_CASE("composition order is scale then rotate then translate") {
    const Transform trs{
        Vec3(1.0f, 2.0f, 3.0f),
        Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), radians(90.0f)),
        Vec3(2.0f, 2.0f, 2.0f)};
    const Mat4 m = composeTRS(trs);
    CHECK(veq(m.transformPoint(Vec3(1.0f, 0.0f, 0.0f)), Vec3(1.0f, 2.0f, 1.0f)));
}

TEST_CASE("determinant of TRS equals product of scales") {
    const Mat4 t = Mat4::translation(Vec3(4.0f, -2.0f, 9.0f));
    const Mat4 r = Mat4::rotationZ(radians(33.0f));
    const Mat4 s = Mat4::scaling(Vec3(1.0f, 2.0f, 3.0f));
    CHECK(feq(t.mul(r).mul(s).determinant(), 6.0f));
}

TEST_CASE("inverse round trip") {
    const Mat4 a = Mat4::translation(Vec3(2.0f, -3.0f, 1.0f))
                     .mul(Mat4::rotationY(radians(41.0f)))
                     .mul(Mat4::rotationX(radians(-17.0f)))
                     .mul(Mat4::scaling(Vec3(1.0f, 2.0f, 0.5f)));
    const Mat4 inv = a.inverse();
    CHECK(meq(a.mul(inv), Mat4::identity()));
    CHECK(meq(inv.mul(a), Mat4::identity()));

    const Vec3 p(0.75f, -1.5f, 2.25f);
    CHECK(veq(inv.transformPoint(a.transformPoint(p)), p));
}

TEST_CASE("transpose") {
    const Mat4 t = Mat4::translation(Vec3(1.0f, 2.0f, 3.0f)).mul(Mat4::scaling(Vec3(1.0f, 2.0f, 4.0f)));
    const Mat4 tt = t.transpose().transpose();
    CHECK(meq(tt, t));

    const Mat4 sym = Mat4{Vec4(1.0f, 2.0f, 3.0f, 4.0f), Vec4(2.0f, 5.0f, 6.0f, 7.0f),
                          Vec4(3.0f, 6.0f, 8.0f, 9.0f), Vec4(4.0f, 7.0f, 9.0f, 10.0f)};
    CHECK(meq(sym.transpose(), sym));
}

TEST_CASE("perspective RH zero-to-one depth") {
    const Mat4 p = Mat4::perspectiveRH_ZO(radians(90.0f), 1.0f, 1.0f, 10.0f);

    CHECK(feq(p.transformPoint(Vec3(0.0f, 0.0f, -1.0f)).z, 0.0f));
    CHECK(feq(p.transformPoint(Vec3(0.0f, 0.0f, -10.0f)).z, 1.0f));

    CHECK(feq(p.transformPoint(Vec3(1.0f, 0.0f, -1.0f)).x, 1.0f));
    CHECK(feq(p.transformPoint(Vec3(-1.0f, 0.0f, -1.0f)).x, -1.0f));
    CHECK(feq(p.transformPoint(Vec3(0.0f, 1.0f, -1.0f)).y, 1.0f));

    const float ndcX = p.transformPoint(Vec3(1.0f, 0.0f, -2.0f)).x;
    CHECK(feq(ndcX, 0.5f));
}

TEST_CASE("forward-Z policy is monotonic from near zero to far one") {
    CHECK_FALSE(kUsesReversedZ);
    const Mat4 projection = Mat4::perspectiveRH_ZO(radians(65.0f), 1.7f, 0.25f, 400.0f);
    const float nearDepth = projection.transformPoint(Vec3(0.0f, 0.0f, -0.25f)).z;
    const float middleDepth = projection.transformPoint(Vec3(0.0f, 0.0f, -10.0f)).z;
    const float farDepth = projection.transformPoint(Vec3(0.0f, 0.0f, -400.0f)).z;

    CHECK(feq(nearDepth, 0.0f));
    CHECK(nearDepth < middleDepth);
    CHECK(middleDepth < farDepth);
    CHECK(feq(farDepth, 1.0f));
}

TEST_CASE("ortho maps box corners to NDC cube") {
    const Mat4 o = Mat4::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.5f, 2.0f);
    const Vec3 nearCorner = o.transformPoint(Vec3(1.0f, 1.0f, -0.5f));
    const Vec3 farCorner = o.transformPoint(Vec3(-1.0f, -1.0f, -2.0f));

    CHECK(veq(nearCorner, Vec3(1.0f, 1.0f, 0.0f)));
    CHECK(veq(farCorner, Vec3(-1.0f, -1.0f, 1.0f)));
}

TEST_CASE("lookAt RH places target down the view axis") {
    const Mat4 view = Mat4::lookAtRH(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));

    CHECK(veq(view.transformPoint(Vec3(0.0f, 0.0f, 0.0f)), Vec3(0.0f, 0.0f, -5.0f)));
    CHECK(veq(view.transformPoint(Vec3(0.0f, 0.0f, 5.0f)), Vec3(0.0f, 0.0f, 0.0f)));
    CHECK(veq(view.transformVector(xAxis), xAxis));

    const Mat4 proj = Mat4::perspectiveRH_ZO(radians(90.0f), 1.0f, 5.0f, 15.0f);
    const Vec3 ndc = proj.mul(view).transformPoint(Vec3(0.0f, 0.0f, 0.0f));
    CHECK(feq(ndc.z, 0.0f));
    CHECK(feq(ndc.x, 0.0f));
    CHECK(feq(ndc.y, 0.0f));
}

TEST_CASE("fromQuat matches rotationZ") {
    const Quat q = Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), radians(30.0f));
    CHECK(meq(Mat4::fromQuat(q), Mat4::rotationZ(radians(30.0f))));
}

TEST_CASE("identity matrix battery") {
    const Mat4 i = Mat4::identity();
    CHECK(feq(i.determinant(), 1.0f, kTightTolerance));
    CHECK(meq(i.transpose(), i));
    CHECK(meq(i.inverse(), i));
    CHECK(meq(i.mul(i), i));
    CHECK(meq(Mat4::fromQuat(Quat::identity()), i));

    const Mat3 i3 = Mat3::identity();
    CHECK(feq(i3.determinant(), 1.0f, kTightTolerance));
    CHECK(meq(i3.inverse(), i3));
}

TEST_CASE("closed form inverses of translation and scaling") {
    const Vec3 t(3.0f, -7.5f, 12.0f);
    F4_CHECK_NEAR(Mat4::translation(t).inverse(), Mat4::translation(-t), kTightTolerance);

    const Vec3 s(2.0f, 4.0f, 0.25f);
    const Mat4 si = Mat4::scaling(s).inverse();
    CHECK(feq(si(0, 0), 0.5f));
    CHECK(feq(si(1, 1), 0.25f));
    CHECK(feq(si(2, 2), 4.0f));
    CHECK(feq(si.determinant(), 1.0f / (2.0f * 4.0f * 0.25f)));
}

TEST_CASE("rotation inverse equals transpose") {
    const Mat4 r = Mat4::rotationZ(radians(33.0f))
                     .mul(Mat4::rotationY(radians(-21.0f)))
                     .mul(Mat4::rotationX(radians(11.0f)));
    F4_CHECK_NEAR(r.inverse(), r.transpose(), kDefaultTolerance);
    F4_CHECK_NEAR(r.mul(r.transpose()), Mat4::identity(), kDefaultTolerance);
}

TEST_CASE("unstructured sheared matrix inverts consistently both orders") {
    const Mat4 a = Mat4{Vec4(2.0f, 1.0f, 0.5f, 3.0f),
                        Vec4(-1.0f, 3.0f, 2.0f, -4.0f),
                        Vec4(0.25f, -2.0f, 5.0f, 1.0f),
                        Vec4(7.0f, 0.0f, -3.0f, 1.0f)};
    const Mat4 inv = a.inverse();

    CHECK(feq(a.determinant() * inv.determinant(), 1.0f, kLooseTolerance));
    F4_CHECK_NEAR(a.mul(inv), Mat4::identity(), kDefaultTolerance);
    F4_CHECK_NEAR(inv.mul(a), Mat4::identity(), kDefaultTolerance);
}

TEST_CASE("composed rotation equals stepwise application") {
    const Mat4 rz = Mat4::rotationZ(radians(40.0f));
    const Mat4 ry = Mat4::rotationY(radians(-65.0f));
    const Mat4 rx = Mat4::rotationX(radians(15.0f));
    const Mat4 combined = rz.mul(ry).mul(rx);
    const Vec3 p(0.75f, -1.25f, 2.0f);

    const Vec3 stepwise = rz.transformVector(ry.transformVector(rx.transformVector(p)));
    F4_CHECK_NEAR(combined.transformVector(p), stepwise, kTightTolerance);
}

TEST_CASE("perspective with non square aspect scales x only") {
    const Mat4 p = Mat4::perspectiveRH_ZO(radians(90.0f), 2.0f, 1.0f, 10.0f);
    const Vec3 ndc = p.transformPoint(Vec3(1.0f, 0.0f, -1.0f));

    CHECK(feq(ndc.x, 0.5f));
    CHECK(feq(ndc.y, 0.0f));
    CHECK(feq(ndc.z, 0.0f));
}

TEST_CASE("singular matrix inverse produces non finite entries") {
    const Mat4 singular = Mat4::scaling(Vec3(1.0f, 1.0f, 0.0f));
    const Mat4 inv = singular.inverse();

    bool anyNonFinite = false;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!std::isfinite(inv(r, c))) {
                anyNonFinite = true;
            }
        }
    }
    CHECK(anyNonFinite);
}

TEST_CASE("full model view projection pipeline lands on hand computed NDC") {
    const Transform modelTrs{
        Vec3(2.0f, 3.0f, 4.0f),
        Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), radians(90.0f)),
        Vec3(2.0f, 2.0f, 2.0f)};
    const Mat4 model = composeTRS(modelTrs);
    const Mat4 view = Mat4::lookAtRH(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = Mat4::perspectiveRH_ZO(radians(90.0f), 1.0f, 1.0f, 10.0f);
    const Mat4 mvp = proj.mul(view).mul(model);

    const Vec3 world = model.transformPoint(Vec3(1.0f, 0.0f, 0.0f));
    F4_CHECK_NEAR(world, Vec3(2.0f, 3.0f, 2.0f));

    const Vec3 viewSpace = view.transformPoint(world);
    F4_CHECK_NEAR(viewSpace, Vec3(2.0f, 3.0f, -3.0f));

    const Vec3 ndc = proj.transformPoint(viewSpace);
    F4_CHECK_NEAR(ndc.x, 2.0f / 3.0f);
    F4_CHECK_NEAR(ndc.y, 1.0f);
    F4_CHECK_NEAR(ndc.z, 20.0f / 27.0f);

    const Vec3 direct = mvp.transformPoint(Vec3(1.0f, 0.0f, 0.0f));
    F4_CHECK_NEAR(direct, ndc, kTightTolerance);
}
