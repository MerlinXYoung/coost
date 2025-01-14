#include "j2s.h"  // generated by:  gen j2s.proto

#include "co/color.h"
#include "co/flag.h"
#include "co/print.h"

int main(int argc, char** argv) {
    flag::parse(argc, argv);

    co::Json v;
    v.add_member("b", false)
        .add_member("i", 23)
        .add_member("s", "hello")
        .add_member("data", co::Json().add_member("ii", 11).add_member("ss", "good"))
        .add_member("ai", co::Json().push_back(1).push_back(2).push_back(3))
        .add_member("ao",
                    co::Json().push_back(co::Json().add_member("xx", 88).add_member("yy", "nice")));

    co::print("Json v:\n", v, '\n');

    xx::XX x;
    x.from_json(v);

    co::print("x.b: ", x.b);
    co::print("x.i: ", x.i);
    co::print("x.s: ", x.s);
    co::print("x.data.ii: ", x.data.ii);
    co::print("x.data.ss: ", x.data.ss);
    co::print("x.ai[0]: ", x.ai[0]);
    co::print("x.ai[1]: ", x.ai[1]);
    co::print("x.ai[2]: ", x.ai[2]);
    co::print("x.ao[0].xx: ", x.ao[0].xx);
    co::print("x.ao[0].yy: ", x.ao[0].yy);

    co::print("\nx.as_json():");
    co::print(x.as_json());

    return 0;
}
