//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "tests_shared.hpp"
#include "filter_test_case_base.hpp"
#include "search/term_filter.hpp"
#include "search/range_filter.hpp"
#include "store/memory_directory.hpp"
#include "store/fs_directory.hpp"
#include "formats/formats_10.hpp"

namespace ir = iresearch;

namespace tests {

class term_filter_test_case : public filter_test_case_base {
protected:
  void by_term_sequential_cost() {
    // add segment
    {
      tests::json_doc_generator gen(
        resource("simple_sequential.json"),
        &tests::generic_json_field_factory);
      add_segment(gen);
    }

    // read segment
    ir::index_reader::ptr rdr = open_reader();

    check_query(ir::by_term(), docs_t{ }, costs_t{0}, *rdr);

    // empty term
    check_query(
      ir::by_term().field("name"),
      docs_t{ }, costs_t{0}, *rdr);

    // empty field
    check_query(
      ir::by_term().term("xyz"),
      docs_t{ }, costs_t{0}, *rdr);

    // search : invalid field
    check_query(
      ir::by_term().field( "invalid_field" ).term( "A" ),
      docs_t{ }, costs_t{0}, *rdr);

    // search : single term
    check_query(
      ir::by_term().field( "name" ).term( "A" ),
      docs_t{ 1 }, costs_t{ 1 }, *rdr);

    { 
      ir::by_term q;
      q.field("name").term("A");
      
      auto prepared = q.prepare(*rdr);
      auto sub = rdr->begin();
      auto docs0 = prepared->execute(*sub);
      auto docs1 = prepared->execute(*sub);
      ASSERT_TRUE(docs0->next());
      ASSERT_EQ(docs0->value(), docs1->seek(docs0->value()));
    }

    // search : all terms
    check_query(
      ir::by_term().field( "same" ).term( "xyz" ),
      docs_t{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 },
      costs_t{ 32 },
      *rdr);

    // search : empty result
    check_query(
      ir::by_term().field( "same" ).term( "invalid_term" ),
      docs_t{}, costs_t{0}, *rdr);
  }

  void by_term_sequential_boost() {
    // add segment
    {
      tests::json_doc_generator gen(
        resource("simple_sequential.json"),
        &tests::generic_json_field_factory);
      add_segment( gen );
    }

    // read segment
    ir::index_reader::ptr rdr = open_reader();

    // create filter
    ir::by_term filter;
    filter.field( "name" ).term( "A" );

    // create order
    iresearch::order ord;
    ord.add<tests::sort::boost>();
    auto pord = ord.prepare();

    // without boost
    {
      auto prep = filter.prepare(*rdr, pord);
      auto docs = prep->execute(*(rdr->begin()), pord);

      const iresearch::score* scr = docs->attributes().get<iresearch::score>();
      ASSERT_NE(nullptr, scr);

      // first hit
      {
        ASSERT_TRUE(docs->next());
        docs->score();
        auto doc_boost = scr->get<tests::sort::boost::score_t>(0);
        ASSERT_EQ(iresearch::boost::boost_t(0), doc_boost);
      }

      ASSERT_FALSE(docs->next());
    }

    // with boost
    {
      const iresearch::boost::boost_t value = 5;
      filter.boost(value);

      auto prep = filter.prepare(*rdr, pord);
      auto docs = prep->execute(*(rdr->begin()), pord);

      const iresearch::score* scr = docs->attributes().get<iresearch::score>();
      ASSERT_NE(nullptr, scr);

      // first hit
      {
        ASSERT_TRUE(docs->next());
        docs->score();
        auto doc_boost = scr->get<tests::sort::boost::score_t>(0);
        ASSERT_EQ(iresearch::boost::boost_t(value), doc_boost);
      }

      ASSERT_FALSE(docs->next());
    }
  }

  void by_term_sequential_numeric() {
    // add segment
    {
      tests::json_doc_generator gen(
        resource("simple_sequential.json"),
        [](tests::document& doc,
           const std::string& name,
           const tests::json::json_value& data) {
          if (data.quoted) {
            doc.insert(std::make_shared<templates::string_field>(
              ir::string_ref(name),
              ir::string_ref(data.value)
            ));
          } else if ("null" == data.value) {
            doc.insert(std::make_shared<tests::binary_field>());
            auto& field = (doc.indexed.end() - 1).as<tests::binary_field>();
            field.name(iresearch::string_ref(name));
            field.value(ir::null_token_stream::value_null());
          } else if ("true" == data.value) {
            doc.insert(std::make_shared<tests::binary_field>());
            auto& field = (doc.indexed.end() - 1).as<tests::binary_field>();
            field.name(iresearch::string_ref(name));
            field.value(ir::boolean_token_stream::value_true());
          } else if ("false" == data.value) {
            doc.insert(std::make_shared<tests::binary_field>());
            auto& field = (doc.indexed.end() - 1).as<tests::binary_field>();
            field.name(iresearch::string_ref(name));
            field.value(ir::boolean_token_stream::value_true());
          } else {
            char* czSuffix;
            double dValue = strtod(data.value.c_str(), &czSuffix);
            if (!czSuffix[0]) {
              // 'value' can be interpreted as a double
              doc.insert(std::make_shared<tests::double_field>());
              auto& field = (doc.indexed.end() - 1).as<tests::double_field>();
              field.name(iresearch::string_ref(name));
              field.value(dValue);
            }

            float fValue = strtof(data.value.c_str(), &czSuffix);
            if (!czSuffix[0]) {
              // 'value' can be interpreted as a float 
              doc.insert(std::make_shared<tests::float_field>());
              auto& field = (doc.indexed.end() - 1).as<tests::float_field>();
              field.name(iresearch::string_ref(name));
              field.value(fValue);
            }

            uint64_t lValue = uint64_t(std::ceil(dValue));
            {
              doc.insert(std::make_shared<tests::long_field>());
              auto& field = (doc.indexed.end() - 1).as<tests::long_field>();
              field.name(iresearch::string_ref(name));
              field.value(lValue);
            }

            {
              doc.insert(std::make_shared<tests::int_field>());
              auto& field = (doc.indexed.end() - 1).as<tests::int_field>();
              field.name(iresearch::string_ref(name));
              field.value(int32_t(lValue));
            }
          }
        });
      add_segment(gen);
    }

    ir::index_reader::ptr rdr = open_reader();

    // long (20)
    {
      ir::numeric_token_stream stream;
      stream.reset(INT64_C(20));
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("seq").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 21 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }

    // int (21)
    {
      ir::numeric_token_stream stream;
      stream.reset(INT32_C(21));
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("seq").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 22 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }

    // double (90.564) 
    {
      ir::numeric_token_stream stream;
      stream.reset((double_t)90.564);
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("value").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 13 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }

    // float (90.564) 
    {
      ir::numeric_token_stream stream;
      stream.reset((float_t)90.564f);
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("value").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 13 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }

    // double (100)
    {
      ir::numeric_token_stream stream;
      stream.reset((double_t)100.);
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("value").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 1, 5, 7, 9, 10 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }

    // float_t(100)
    {
      ir::numeric_token_stream stream;
      stream.reset((float_t)100.f);
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("value").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 1, 5, 7, 9, 10 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }

    // int(100)
    {
      ir::numeric_token_stream stream;
      stream.reset(100);
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("value").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 1, 5, 7, 9, 10 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }

    // long(100)
    {
      ir::numeric_token_stream stream;
      stream.reset(INT64_C(100));
      const ir::term_attribute* term = stream.attributes().get<ir::term_attribute>();
      ASSERT_TRUE(stream.next());

      ir::by_term query;
      query.field("value").term(term->value());

      auto prepared = query.prepare(*rdr);

      std::vector<ir::doc_id_t> expected { 1, 5, 7, 9, 10 };
      std::vector<ir::doc_id_t> actual;

      for (const auto& sub : *rdr) {
        auto docs = prepared->execute(sub); 
        for (;docs->next();) {
          actual.push_back(docs->value());
        }
      }
      ASSERT_EQ(expected, actual);
    }
  }

  void by_term_sequential() {
    // add segment
    {
      tests::json_doc_generator gen(
        resource("simple_sequential.json"),
        &tests::generic_json_field_factory);
      add_segment( gen );
    }

    ir::index_reader::ptr rdr = open_reader();

    // empty query
    check_query( ir::by_term(), docs_t{ }, *rdr );

    // empty term
    check_query( 
      ir::by_term().field("name"), 
      docs_t{ }, *rdr );

    // empty field
    check_query( 
      ir::by_term().term("xyz"), 
      docs_t{ }, *rdr );

    // search : invalid field
    check_query( 
      ir::by_term().field( "invalid_field" ).term( "A" ),
      docs_t{ }, *rdr );

    // search : single term
    check_query( 
      ir::by_term().field( "name" ).term( "A" ),
      docs_t{ 1 }, *rdr );

    // search : all terms
    check_query(
      ir::by_term().field( "same" ).term( "xyz" ),
      docs_t{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 },
      *rdr );

    // search : empty result
    check_query(
      ir::by_term().field( "same" ).term( "invalid_term" ),
      docs_t{}, *rdr );
  }

  void by_term_schemas() {
    // write segments
    {
      auto writer = open_writer(iresearch::OM_CREATE);

      std::vector<doc_generator_base::ptr> gens;
      gens.emplace_back(new tests::json_doc_generator(
        resource("AdventureWorks2014.json"),
        &tests::generic_json_field_factory
      ));
      gens.emplace_back(new tests::json_doc_generator(
        resource("AdventureWorks2014Edges.json"),
        &tests::generic_json_field_factory
      ));
      gens.emplace_back(new tests::json_doc_generator(
        resource("Northwnd.json"),
        &tests::generic_json_field_factory
      ));
      gens.emplace_back(new tests::json_doc_generator(
        resource("NorthwndEdges.json"),
        &tests::generic_json_field_factory
      ));
      gens.emplace_back(new tests::json_doc_generator(
        resource("Northwnd.json"),
        &tests::generic_json_field_factory
      ));
      gens.emplace_back(new tests::json_doc_generator(
        resource("NorthwndEdges.json"),
        &tests::generic_json_field_factory
      ));
      add_segments(*writer, gens);
    }

    ir::index_reader::ptr rdr = open_reader();
    check_query(
      ir::by_term().field( "Fields" ).term( "FirstName" ),
      docs_t{ 28, 167, 194 }, *rdr );

    // address to the [SDD-179]
    check_query(
      ir::by_term().field( "Name" ).term( "Product" ),
      docs_t{ 32 }, *rdr );
  }
};

} // tests

// ----------------------------------------------------------------------------
// --SECTION--                                               by_term base tests 
// ----------------------------------------------------------------------------

TEST(by_term_test, ctor) {
  ir::by_term q;
  ASSERT_EQ(ir::by_term::type(), q.type());
  ASSERT_TRUE(q.term().empty());
  ASSERT_EQ("", q.field());
  ASSERT_EQ(ir::boost::no_boost(), q.boost());
}

TEST(by_term_test, equal) { 
  ir::by_term q;
  q.field("field").term("term");
  ASSERT_EQ(q, ir::by_term().field("field").term("term"));
  ASSERT_NE(q, ir::by_term().field("field1").term("term"));
}

TEST(by_term_test, boost) {
  // no boost
  {
    ir::by_term q;
    q.field("field").term("term");

    auto prepared = q.prepare(tests::empty_index_reader::instance());
    ASSERT_EQ(ir::boost::no_boost(), ir::boost::extract(prepared->attributes()));
  }

  // with boost
  {
    iresearch::boost::boost_t boost = 1.5f;
    ir::by_term q;
    q.field("field").term("term");
    q.boost(boost);

    auto prepared = q.prepare(tests::empty_index_reader::instance());
    ASSERT_EQ(boost, ir::boost::extract(prepared->attributes()));
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                           memory_directory + iresearch_format_10
// ----------------------------------------------------------------------------

class memory_term_filter_test_case : public tests::term_filter_test_case {
protected:
  virtual ir::directory* get_directory() override {
    return new ir::memory_directory();
  }

  virtual ir::format::ptr get_codec() override {
    static ir::version10::format FORMAT;
    return ir::format::ptr(&FORMAT, [](ir::format*)->void{});
  }
};

TEST_F(memory_term_filter_test_case, by_term) {
  by_term_sequential();  
  by_term_schemas();
}

TEST_F(memory_term_filter_test_case, by_term_numeric) {
  by_term_sequential_numeric();
}

TEST_F(memory_term_filter_test_case, by_term_boost) {
  by_term_sequential_boost();
}

TEST_F(memory_term_filter_test_case, by_term_cost) {
  by_term_sequential_cost();
}

// ----------------------------------------------------------------------------
// --SECTION--                               fs_directory + iresearch_format_10
// ----------------------------------------------------------------------------

class fs_term_filter_test_case : public tests::term_filter_test_case {
protected:
  virtual ir::directory* get_directory() override {
    const fs::path dir = fs::path( test_dir() ).append( "index" );
    return new iresearch::fs_directory(dir.string());
  }

  virtual ir::format::ptr get_codec() override {
    return ir::formats::get("1_0");
  }
};

TEST_F(fs_term_filter_test_case, by_term) {
  by_term_sequential();
  by_term_schemas();
}

TEST_F(fs_term_filter_test_case, by_term_boost) {
  by_term_sequential_boost();
}

TEST_F(fs_term_filter_test_case, by_term_numeric) {
  by_term_sequential_numeric();
}

TEST_F(fs_term_filter_test_case, by_term_cost) {
  by_term_sequential_cost();
}
