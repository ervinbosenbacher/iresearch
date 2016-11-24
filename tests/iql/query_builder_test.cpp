//
// IResearch search engine 
// 
// Copyright � 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include <boost/locale/generator.hpp>

#include "gtest/gtest.h"
#include "tests_config.hpp"
#include "tests_shared.hpp"
#include "analysis/analyzers.hpp"
#include "analysis/token_streams.hpp"
#include "formats/formats_10.hpp"
#include "index/doc_generator.hpp"
#include "index/index_reader.hpp"
#include "index/index_writer.hpp"
#include "index/index_tests.hpp"
#include "iql/query_builder.hpp"
#include "search/boolean_filter.hpp"
#include "search/scorers.hpp"
#include "search/term_filter.hpp"
#include "utils/runtime_utils.hpp"

namespace tests {
  class test_sort: public iresearch::sort {
   public:
    DECLARE_SORT_TYPE();
    DECLARE_FACTORY_DEFAULT();

    class prepared : sort::prepared {
     public:
      DECLARE_FACTORY(prepared);
      prepared() { }
      virtual collector::ptr prepare_collector() const override { return nullptr; }
      virtual scorer::ptr prepare_scorer(
          const iresearch::sub_reader&,
          const iresearch::term_reader&,
          const iresearch::attributes& query_attrs, 
          const iresearch::attributes& doc_attrs) const override { 
        return nullptr; 
      }
      virtual const iresearch::flags& features() const override { 
        return iresearch::flags::empty_instance();
      }
      virtual void prepare_score(iresearch::byte_type* score) const override {}
      virtual void add(iresearch::byte_type* dst, const iresearch::byte_type* src) const override {}
      virtual bool less(const iresearch::byte_type* lhs, const iresearch::byte_type* rhs) const override { throw std::bad_function_call(); }
      virtual size_t size() const { return 0; }
    };

    test_sort():sort(test_sort::type()) {}
    virtual sort::prepared::ptr prepare() const { return test_sort::prepared::make<test_sort::prepared>(); }
  };

  DEFINE_SORT_TYPE(test_sort);
  DEFINE_FACTORY_SINGLETON(test_sort);

  class IqlQueryBuilderTestSuite: public ::testing::Test {
    virtual void SetUp() {
      // Code here will be called immediately after the constructor (right before each test).
      // use the following code to enble parser debug outut
      //::iresearch::iql::debug(parser, [true|false]);

      // ensure stopwords are loaded/cached for the 'en' locale used for text analysis below
      {
        // same env variable name as iresearch::analysis::text_token_stream::STOPWORD_PATH_ENV_VARIABLE
        const auto text_stopword_path_var = "IRESEARCH_TEXT_STOPWORD_PATH";
        const char* czOldStopwordPath = iresearch::getenv(text_stopword_path_var);
        std::string sOldStopwordPath = czOldStopwordPath == nullptr ? "" : czOldStopwordPath;

        iresearch::setenv(text_stopword_path_var, IResearch_test_resource_dir, true);

        auto locale = boost::locale::generator().generate("en");
        const std::string tmp_str;

        iresearch::analysis::analyzers::get("text", "en"); // stream needed only to load stopwords

        if (czOldStopwordPath) {
          iresearch::setenv(text_stopword_path_var, sOldStopwordPath.c_str(), true);
        }
      }
    }

    virtual void TearDown() {
      // Code here will be called immediately after each test (right before the destructor).
    }
  };

  class analyzed_string_field: public templates::string_field {
   public:
    analyzed_string_field(const iresearch::string_ref& name, const iresearch::string_ref& value):
      templates::string_field(name, value, true, true),
      token_stream_(ir::analysis::analyzers::get("text", "en")) {
      if (!token_stream_) {
        throw std::runtime_error("Failed to get 'text' analyzer for args: en");
      }
    }
    virtual ~analyzed_string_field() {}
    virtual iresearch::token_stream* get_tokens() const override {
      const iresearch::string_ref& value = this->value();
      token_stream_->reset(value);
      return token_stream_.get();
    }
      
   private:
    static std::unordered_set<std::string> ignore_set_; // there are no stopwords for the 'c' locale in tests
    iresearch::analysis::analyzer::ptr token_stream_;
  };

  std::unordered_set<std::string> analyzed_string_field::ignore_set_;

  iresearch::directory_reader::ptr load_json(
    iresearch::directory& dir,
    const std::string json_resource,
    bool analyze_text = false
  ) {

    static auto analyzed_field_factory = [](
        tests::document& doc,
        const std::string& name,
        const tests::json::json_value& value) {
      doc.add(new analyzed_string_field(
        iresearch::string_ref(name),
        iresearch::string_ref(value.value)
      ));
    };

    static auto generic_field_factory = [](
        tests::document& doc,
        const std::string& name,
        const tests::json::json_value& data) {
      if (data.quoted) {
        doc.add(new templates::string_field(
          ir::string_ref(name),
          ir::string_ref(data.value),
          true, true));
      } else if ("null" == data.value) {
        doc.add(new tests::binary_field());
        auto& field = (doc.end() - 1).as<tests::binary_field>();
        field.name(iresearch::string_ref(name));
        field.value(ir::null_token_stream::value_null());
      } else if ("true" == data.value) {
        doc.add(new tests::binary_field());
        auto& field = (doc.end() - 1).as<tests::binary_field>();
        field.name(iresearch::string_ref(name));
        field.value(ir::boolean_token_stream::value_true());
      } else if ("false" == data.value) {
        doc.add(new tests::binary_field());
        auto& field = (doc.end() - 1).as<tests::binary_field>();
        field.name(iresearch::string_ref(name));
        field.value(ir::boolean_token_stream::value_true());
      } else {
        char* czSuffix;
        double dValue = strtod(data.value.c_str(), &czSuffix);

        // 'value' can be interpreted as a double
        if (!czSuffix[0]) {
          doc.add(new tests::double_field());
          auto& field = (doc.end() - 1).as<tests::double_field>();
          field.name(iresearch::string_ref(name));
          field.value(dValue);
        }
      }
    };

    iresearch::version10::format codec;
    iresearch::format::ptr codec_ptr(&codec, [](iresearch::format*)->void{});
    auto writer =
      iresearch::index_writer::make(dir, codec_ptr, iresearch::OPEN_MODE::OM_CREATE);
    json_doc_generator generator(
      test_base::resource(json_resource), 
      analyze_text ? analyzed_field_factory : &tests::generic_json_field_factory);
    const document* doc;

    while ((doc = generator.next()) != nullptr) {
      writer->insert(doc->begin(), doc->end());
    }

    writer->commit();

    return iresearch::directory_reader::open(dir, codec_ptr);
  }
}

using namespace tests;
using namespace iresearch::iql;

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(IqlQueryBuilderTestSuite, test_query_builder) {
  sequence_function::contextual_function_t fnNum = [](
    sequence_function::contextual_buffer_t& buf,
    const std::locale& locale,
    void* cookie,
    const sequence_function::contextual_function_args_t& args
  )->bool {
    iresearch::bstring value;
    bool bValue;

    args[0].value(value, bValue, locale, cookie);

    double dValue = strtod(iresearch::ref_cast<char>(value).c_str(), nullptr);
    iresearch::numeric_token_stream stream;
    stream.reset((double_t)dValue);
    auto& term = stream.attributes().get<iresearch::term_attribute>();

    while (stream.next()) {
      buf.append(term->value());
    }

    return true;
  };
  iresearch::memory_directory dir;
  auto reader = load_json(dir, "simple_sequential.json");
  ASSERT_EQ(1, reader->size());

  auto& segment = (*reader)[0]; // assume 0 is id of first/only segment

  // single string term
  {
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    const iresearch::string_ref expected_name = "name";
    iresearch::string_ref expected_value;
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value != expected_value) {
        return false;
      }

      return true;
    };

    auto query = query_builder().build("name==A", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());

    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }
/* FIXME reenable once bug is fixed
  // single numeric term
  {
    sequence_function seq_function(fnNum, 1);
    sequence_functions seq_functions = {
      { "#", seq_function },
    };
    functions functions(seq_functions);
    auto query = query_builder(functions).build("seq==#(0)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    tests::field_visitor visitor("name");
    segment.document(docsItr->value(), visitor);
    ASSERT_EQ(std::string("A"), visitor.v_string);
    ASSERT_FALSE(docsItr->next());
  }
*/
  // term negation
  {
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    const iresearch::string_ref expected_name = "name";
    iresearch::string_ref unexpected_value;
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &unexpected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value == unexpected_value) {
        return false;
      }

      return true;
    };

    auto query = query_builder().build("name!=A", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    for (size_t count = segment.docs_count() - 1; count > 0; --count) {
      ASSERT_TRUE(docsItr->next());
      unexpected_value = "A";
      ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    }

    ASSERT_FALSE(docsItr->next());
  }

  // term union
  {
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    const iresearch::string_ref expected_name = "name";
    iresearch::string_ref expected_value;
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value != expected_value) {
        return false;
      }

      return true;
    };

    auto query = query_builder().build("name==A || name==B OR name==C", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_TRUE(docsItr->next());
    expected_value = "B";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_TRUE(docsItr->next());
    expected_value = "C";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }
/* FIXME reenable once bug is fixed
  // term intersection
  {
    sequence_function seq_function(fnNum, 1);
    sequence_functions seq_functions = {
      { "#", seq_function },
    };
    functions functions(seq_functions);
    auto query = query_builder(functions).build("name==A && seq==#(0) AND same==xyz", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    tests::field_visitor visitor("name");
    segment.document(docsItr->value(), visitor);
    ASSERT_EQ(std::string("A"), visitor.v_string);
    ASSERT_FALSE(docsItr->next());
  }
*/
  // single term greater ranges
  {
    std::unordered_set<std::string> expected = { "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
    
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    double_t seq;
    iresearch::index_reader::document_visitor_f visitorSeq = [&codecs, &seq](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "seq") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      seq = iresearch::read_zvdouble(in);
      return true;
    };
    
    iresearch::index_reader::document_visitor_f visitorName = [&expected, &codecs](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "name") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (1 != expected.erase(value)) {
        return false;
      }

      return true;
    };
   
    auto query = query_builder().build("name > M", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    while (docsItr->next()) {
      ASSERT_TRUE(segment.document(docsItr->value(), visitorSeq));

      if (seq < 26) { // validate only first 26 records [A-Z]
        ASSERT_TRUE(segment.document(docsItr->value(), visitorName));
      }
    }

    ASSERT_TRUE(expected.empty());
  }

  // single term greater-equal ranges
  {
    std::unordered_set<std::string> expected = { "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
    
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    double_t seq;
    iresearch::index_reader::document_visitor_f visitorSeq = [&codecs, &seq](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "seq") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      seq = iresearch::read_zvdouble(in);
      return true;
    };

    iresearch::index_reader::document_visitor_f visitorName = [&expected, &codecs](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "name") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (1 != expected.erase(value)) {
        return false;
      }

      return true;
    };
    auto query = query_builder().build("name >= M", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    while (docsItr->next()) {
      ASSERT_TRUE(segment.document(docsItr->value(), visitorSeq));

      if (seq < 26) { // validate only first 26 records [A-Z]
        ASSERT_TRUE(segment.document(docsItr->value(), visitorName));
      }
    }

    ASSERT_TRUE(expected.empty());
  }

  // single term lesser-equal ranges
  {
    std::unordered_set<std::string> expected = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N" };

    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    double_t seq;
    iresearch::index_reader::document_visitor_f visitorSeq = [&codecs, &seq](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "seq") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      seq = iresearch::read_zvdouble(in);
      return true;
    };
    
    iresearch::index_reader::document_visitor_f visitorName = [&expected, &codecs](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "name") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (1 != expected.erase(value)) {
        return false;
      }

      return true;
    };
    auto query = query_builder().build("name <= N", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    while (docsItr->next()) {
      ASSERT_TRUE(segment.document(docsItr->value(), visitorSeq));

      if (seq < 26) { // validate only first 26 records [A-Z]
        ASSERT_TRUE(segment.document(docsItr->value(), visitorName));
      }
    }

    ASSERT_TRUE(expected.empty());
  }

  // single term lesser ranges
  {
    std::unordered_set<std::string> expected = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M" };
    
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    double_t seq;
    iresearch::index_reader::document_visitor_f visitorSeq = [&codecs, &seq](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "seq") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      seq = iresearch::read_zvdouble(in);
      return true;
    };
    
    iresearch::index_reader::document_visitor_f visitorName = [&expected, &codecs](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "name") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (1 != expected.erase(value)) {
        return false;
      }

      return true;
    };
    auto query = query_builder().build("name < N", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    while (docsItr->next()) {
      ASSERT_TRUE(segment.document(docsItr->value(), visitorSeq));

      if (seq < 26) { // validate only first 26 records [A-Z]
        ASSERT_TRUE(segment.document(docsItr->value(), visitorName));
      }
    }

    ASSERT_TRUE(expected.empty());
  }

  // limit
  {
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    const iresearch::string_ref expected_name = "name";
    iresearch::string_ref expected_value;
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value != expected_value) {
        return false;
      }

      return true;
    };

    auto query = query_builder().build("name==A limit 42", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_NE(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());

    ASSERT_EQ(42, *(query.limit));
  }

  // ...........................................................................
  // invalid
  // ...........................................................................

  // invalid query
  {
    auto query = query_builder().build("name==A bcd", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_NE(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_EQ(nullptr, pQuery.get());

    ASSERT_EQ(0, query.error->find("@([8 - 11], 11): syntax error"));
  }

  // valid query with missing dependencies (e.g. missing functions)
  {
    auto query = query_builder().build("name(A)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_NE(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_EQ(nullptr, pQuery.get());

    ASSERT_EQ(std::string("@(7): parse error"), *(query.error));
  }

  // unsupported functionality by iResearch queries (e.g. like, ranges)
  {
    query_builder::branch_builder_function_t fnFail = [](
      boolean_function::contextual_buffer_t&,
      const std::locale&,
      const iresearch::string_ref&,
      void* cookie,
      const boolean_function::contextual_function_args_t&
    )->bool {
      return false;
    };
    query_builder::branch_builders builders(&fnFail, nullptr, nullptr, nullptr, nullptr);
    auto query = query_builder(builders).build("name==(A, bcd)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_NE(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader, iresearch::order::prepared::unordered());
    ASSERT_EQ(nullptr, pQuery.get());

    ASSERT_EQ(std::string("filter conversion error, node: @5\n('name'@2 == ('A'@3, 'bcd'@4)@5)@6"), *(query.error));
  }
}

TEST_F(IqlQueryBuilderTestSuite, test_query_builder_builders_default) {
  std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
    { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
  };

  const iresearch::string_ref expected_name = "name";
  iresearch::string_ref expected_value;
  iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
    const iresearch::field_meta& field, iresearch::data_input& in
  ) {
    if (field.name != expected_name) {
      auto it = codecs.find(field.name);
      if (codecs.end() == it) {
        return false; // can't find codec
      }
      it->second(in); // skip field
      return true;
    }

    auto value = iresearch::read_string<std::string>(in);
    if (value != expected_value) {
      return false;
    }

    return true;
  };

  iresearch::memory_directory dir;
  auto reader = load_json(dir, "simple_sequential.json");
  ASSERT_EQ(1, reader->size());

  auto& segment = (*reader)[0]; // assume 0 is id of first/only segment

  // default range builder functr ()
  {
    query_builder::branch_builders builders;
    auto query = query_builder(builders).build("name==(A, C)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "B";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // default range builder functr (]
  {
    query_builder::branch_builders builders;
    auto query = query_builder(builders).build("name==(A, B]", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "B";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // default range builder functr [)
  {
    query_builder::branch_builders builders;
    auto query = query_builder(builders).build("name==[A, B)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // default range builder functr []
  {
    query_builder::branch_builders builders;
    auto query = query_builder(builders).build("name==[A, B]", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_TRUE(docsItr->next());
    expected_value = "B";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // default similar '~=' operator
  {
    iresearch::memory_directory analyzed_dir;
    auto analyzed_reader = load_json(analyzed_dir, "simple_sequential.json", true);
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        iresearch::read_string<std::string>(in); // in analyzed case we treat all fields as strings
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value != expected_value) {
        return false;
      }

      return true;
    };

    ASSERT_EQ(1, analyzed_reader->size());
    auto& analyzed_segment = (*analyzed_reader)[0]; // assume 0 is id of first/only segment

    query_builder::branch_builders builders;
    auto locale = boost::locale::generator().generate("en"); // a locale that exists in tests
    auto query = query_builder(builders).build("name~=B", locale);
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*analyzed_reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(analyzed_segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "B";
    ASSERT_TRUE(analyzed_segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }
}

TEST_F(IqlQueryBuilderTestSuite, test_query_builder_builders_custom) {
  std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
    { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
  };

  const iresearch::string_ref expected_name = "name";
  iresearch::string_ref expected_value;
  iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
    const iresearch::field_meta& field, iresearch::data_input& in
  ) {
    if (field.name != expected_name) {
      auto it = codecs.find(field.name);
      if (codecs.end() == it) {
        return false; // can't find codec
      }
      it->second(in); // skip field
      return true;
    }

    auto value = iresearch::read_string<std::string>(in);
    if (value != expected_value) {
      return false;
    }

    return true;
  };

  query_builder::branch_builder_function_t fnFail = [](
    boolean_function::contextual_buffer_t&,
    const std::locale&,
    const iresearch::string_ref&,
    void* cookie,
    const boolean_function::contextual_function_args_t&
  )->bool {
    std::cerr << "File: " << __FILE__ << " Line: " << __LINE__ << " Failed" << std::endl;
    throw "Fail";
  };
  query_builder::branch_builder_function_t fnEqual = [](
    boolean_function::contextual_buffer_t& node,
    const std::locale& locale,
    const iresearch::string_ref& field,
    void* cookie,
    const boolean_function::contextual_function_args_t& args
  )->bool {
    iresearch::bstring value;
    bool bValue;
    args[0].value(value, bValue, locale, cookie);
    node.proxy<iresearch::by_term>().field(field).term(std::move(value));
    return true;
  };
  iresearch::memory_directory dir;
  auto reader = load_json(dir, "simple_sequential.json");
  ASSERT_EQ(1, reader->size());

  auto& segment = (*reader)[0]; // assume 0 is id of first/only segment

  // custom range builder functr ()
  {
    query_builder::branch_builders builders(&fnEqual, &fnFail, &fnFail, &fnFail, &fnFail);
    auto query = query_builder(builders).build("name==(A, B)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // custom range builder functr (]
  {
    query_builder::branch_builders builders(&fnFail, &fnEqual, &fnFail, &fnFail, &fnFail);
    auto query = query_builder(builders).build("name==(A, B]", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // custom range builder functr [)
  {
    query_builder::branch_builders builders(&fnFail, &fnFail, &fnEqual, &fnFail, &fnFail);
    auto query = query_builder(builders).build("name==[A, B)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // custom range builder functr []
  {
    query_builder::branch_builders builders(&fnFail, &fnFail, &fnFail, &fnEqual, &fnFail);
    auto query = query_builder(builders).build("name==[A, B]", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // custom similar '~=' operator
  {
    query_builder::branch_builders builders(&fnFail, &fnFail, &fnFail, &fnFail, &fnEqual);
    auto query = query_builder(builders).build("name~=A", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }
}

TEST_F(IqlQueryBuilderTestSuite, test_query_builder_bool_fns) {
  std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
    { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
  };

  boolean_function::contextual_function_t fnEqual = [](
    boolean_function::contextual_buffer_t& node,
    const std::locale& locale,
    void* cookie,
    const boolean_function::contextual_function_args_t& args
  )->bool {
    iresearch::bstring field;
    iresearch::bstring value;
    bool bField;
    bool bValue;
    args[0].value(field, bField, locale, cookie);
    args[1].value(value, bValue, locale, cookie);
    node.proxy<iresearch::by_term>().field(iresearch::ref_cast<char>(field)).term(std::move(value));
    return true;
  };
  iresearch::memory_directory dir;
  auto reader = load_json(dir, "simple_sequential.json");
  ASSERT_EQ(1, reader->size());

  auto& segment = (*reader)[0]; // assume 0 is id of first/only segment

  // user supplied boolean_function
  {
    const iresearch::string_ref expected_name = "name";
    iresearch::string_ref expected_value;
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value != expected_value) {
        return false;
      }

      return true;
    };

    boolean_function bool_function(fnEqual, 2);
    boolean_functions bool_functions = {
      { "~", bool_function },
    };
    functions functions(bool_functions);
    auto query = query_builder(functions).build("~(name, A)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }

  // user supplied negated boolean_function
  {
    const iresearch::string_ref expected_name = "name";
    iresearch::string_ref unexpected_value;
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &unexpected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value == unexpected_value) {
        return false;
      }

      return true;
    };

    boolean_function bool_function(fnEqual, 2);
    boolean_functions bool_functions = {
      { "~", bool_function },
    };
    functions functions(bool_functions);
    auto query = query_builder(functions).build("!~(name, A)", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    unexpected_value = "A";
    for (size_t count = segment.docs_count() - 1; count > 0; --count) {
      ASSERT_TRUE(docsItr->next());
      ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    }

    ASSERT_FALSE(docsItr->next());
  }

  // user supplied boolean_function with expression args
  {
    boolean_function::contextual_function_t fnCplx = [](
      boolean_function::contextual_buffer_t& node,
      const std::locale& locale,
      void* cookie,
      const boolean_function::contextual_function_args_t& args
    )->bool {
      auto& root = node.proxy<iresearch::Or>();
      iresearch::bstring value;
      bool bValueNil;

      if (args.size() != 3 ||
          !args[0].value(value, bValueNil, locale, cookie) ||
          bValueNil ||
          !args[1].branch(root.add<iresearch::iql::proxy_filter>(), locale, cookie) ||
          !args[2].branch(root.add<iresearch::iql::proxy_filter>(), locale, cookie)) {
        return false;
      }

      root.add<iresearch::by_term>().field("name").term(std::move(value));

      return true;
    };
    boolean_function bool_function(fnCplx, 0, true);
    boolean_function expr_function(fnEqual, 2);
    boolean_functions bool_functions = {
      { "cplx", bool_function },
      { "expr", expr_function },
    };
    functions functions(bool_functions);
    auto query = query_builder(functions).build("cplx(A, (name==C || name==D), expr(name, E))", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    std::unordered_set<std::string> expected = { "A", "C", "D", "E" };
    iresearch::index_reader::document_visitor_f visitor = [&expected, &codecs](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != "name") {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (1 != expected.erase(value)) {
        return false;
      }

      return true;
    };

    while (docsItr->next()) {
      ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    }

    ASSERT_TRUE(expected.empty());
  }
}

TEST_F(IqlQueryBuilderTestSuite, test_query_builder_sequence_fns) {
  std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
    { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
    { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
  };

  const iresearch::string_ref expected_name = "name";
  iresearch::string_ref expected_value;
  iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
    const iresearch::field_meta& field, iresearch::data_input& in
  ) {
    if (field.name != expected_name) {
      auto it = codecs.find(field.name);
      if (codecs.end() == it) {
        return false; // can't find codec
      }
      it->second(in); // skip field
      return true;
    }

    auto value = iresearch::read_string<std::string>(in);
    if (value != expected_value) {
      return false;
    }

    return true;
  };

  sequence_function::contextual_function_t fnValue = [](
    sequence_function::contextual_buffer_t& buf,
    const std::locale&,
    void*,
    const sequence_function::contextual_function_args_t& args
  )->bool {
    iresearch::bytes_ref value(reinterpret_cast<const iresearch::byte_type*>("A"), 1);
    buf.append(value);
    return true;
  };
  iresearch::memory_directory dir;
  auto reader = load_json(dir, "simple_sequential.json");
  ASSERT_EQ(1, reader->size());

  auto& segment = (*reader)[0]; // assume 0 is id of first/only segment

  // user supplied sequence_function
  {
    sequence_function seq_function(fnValue);
    sequence_functions seq_functions = {
      { "valueA", seq_function },
    };
    functions functions(seq_functions);
    auto query = query_builder(functions).build("name==valueA()", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());
    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());
  }
}

TEST_F(IqlQueryBuilderTestSuite, test_query_builder_order) {
  iresearch::memory_directory dir;
  auto reader = load_json(dir, "simple_sequential.json");
  ASSERT_EQ(1, reader->size());

  auto& segment = (*reader)[0]; // assume 0 is id of first/only segment
/* FIXME field-value order is not yet supported by iresearch::search
  // order by sequence
  {
    auto query = query_builder().build("name==A || name == B order seq desc", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_NE(nullptr, query.order);
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);

    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    tests::field_visitor visitor("name");
    ASSERT_TRUE(docsItr->next());
    segment.document(docsItr->value(), visitor);
    ASSERT_EQ(std::string("B"), visitor.v_string);
    ASSERT_TRUE(docsItr->next());
    segment.document(docsItr->value(), visitor);
    ASSERT_EQ(std::string("A"), visitor.v_string);
    ASSERT_FALSE(docsItr->next());
  }

  // custom deterministic order function
  {
    order_function::deterministic_function_t fnTest = [](
      order_function::deterministic_buffer_t& buf,
      const order_function::deterministic_function_args_t& args
    )->bool {
      buf.append("name");
      return true;
    };
    order_function order_function(fnTest);
    order_functions order_functions = {
      { "c", order_function },
    };
    functions functions(order_functions);
    auto query = query_builder(functions).build("name==A || name == B order c() desc", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_NE(nullptr, query.order);
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);

    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    tests::field_visitor visitor("name");
    ASSERT_TRUE(docsItr->next());
    segment.document(docsItr->value(), visitor);
    ASSERT_EQ(std::string("B"), visitor.v_string);
    ASSERT_TRUE(docsItr->next());
    segment.document(docsItr->value(), visitor);
    ASSERT_EQ(std::string("A"), visitor.v_string);
    ASSERT_FALSE(docsItr->next());
  }
*/
  // custom contextual order function
  {
    std::unordered_map<iresearch::string_ref, std::function<void(iresearch::data_input&)>> codecs{
      { "name", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "same", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "duplicated", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "prefix", [](iresearch::data_input& in)->void{ iresearch::read_string<std::string>(in); } },
      { "seq", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
      { "value", [](iresearch::data_input& in)->void{ iresearch::read_zvdouble(in); } },
    };

    const iresearch::string_ref expected_name = "name";
    iresearch::string_ref expected_value;
    iresearch::index_reader::document_visitor_f visitor = [&codecs, &expected_value, &expected_name](
      const iresearch::field_meta& field, iresearch::data_input& in
    ) {
      if (field.name != expected_name) {
        auto it = codecs.find(field.name);
        if (codecs.end() == it) {
          return false; // can't find codec
        }
        it->second(in); // skip field
        return true;
      }

      auto value = iresearch::read_string<std::string>(in);
      if (value != expected_value) {
        return false;
      }

      return true;
    };

    std::vector<std::pair<bool, std::string>> direction;
    sequence_function::deterministic_function_t fnTestSeq = [](
      sequence_function::deterministic_buffer_t& buf,
      const order_function::deterministic_function_args_t&
    )->bool {
      buf.append("xyz");
      return true;
    };
    order_function::contextual_function_t fnTest = [&direction](
      order_function::contextual_buffer_t& buf,
      const std::locale& locale,
      void* cookie,
      const bool& ascending,
      const order_function::contextual_function_args_t& args
    )->bool {
      std::stringstream out;

      for (auto& arg: args) {
        iresearch::bstring value;
        bool bValue;
        arg.value(value, bValue, locale, cookie);
        out << iresearch::ref_cast<char>(value) << "|";
      }

      direction.emplace_back(ascending, out.str());
      buf.add<test_sort>();

      return true;
    };
    order_function order_function0(fnTest, 0);
    order_function order_function2(fnTest, 2);
    order_functions order_functions = {
      { "c", order_function0 },
      { "d", order_function2 },
    };
    sequence_function sequence_function(fnTestSeq);
    sequence_functions sequence_functions = {
      { "e", sequence_function },
    };
    functions functions(sequence_functions, order_functions);
    auto query = query_builder(functions).build("name==A order c() desc, d(e(), f) asc", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_NE(nullptr, query.order);
    ASSERT_EQ(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);

    ASSERT_NE(nullptr, pQuery.get());

    auto docsItr = pQuery->execute(segment);
    ASSERT_NE(nullptr, docsItr.get());

    ASSERT_TRUE(docsItr->next());
    expected_value = "A";
    ASSERT_TRUE(segment.document(docsItr->value(), visitor));
    ASSERT_FALSE(docsItr->next());

    ASSERT_EQ(2, direction.size());
    ASSERT_FALSE(direction[0].first);
    ASSERT_EQ("", direction[0].second);
    ASSERT_TRUE(direction[1].first);
    ASSERT_EQ("xyz|f|", direction[1].second);
  }

  // ...........................................................................
  // invalid
  // ...........................................................................

  // non-existent function
  {
    auto query = query_builder().build("name==A order b()", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.order);
    ASSERT_NE(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_EQ(nullptr, pQuery.get());

    ASSERT_EQ(0, query.error->find("@(17): parse error"));
  }

  // failed deterministic function
  {
    order_function::deterministic_function_t fnFail = [](
      order_function::deterministic_buffer_t&,
      const order_function::deterministic_function_args_t&
    )->bool {
      return false;
    };
    order_function order_function(fnFail);
    order_functions order_functions = {
      { "b", order_function },
    };
    functions functions(order_functions);
    auto query = query_builder(functions).build("name==A order b()", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.order);
    ASSERT_NE(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_EQ(nullptr, pQuery.get());

    ASSERT_EQ(0, query.error->find("order conversion error, node: @7\n'b'()@7 ASC"));
  }

  // failed contextual function
  {
    order_function::contextual_function_t fnFail = [](
      order_function::contextual_buffer_t&,
      const std::locale&,
      void*,
      const bool&,
      const order_function::contextual_function_args_t&
    )->bool {
      return false;
    };
    order_function order_function(fnFail);
    order_functions order_functions = {
      { "b", order_function },
    };
    functions functions(order_functions);
    auto query = query_builder(functions).build("name==A order b()", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.order);
    ASSERT_NE(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_EQ(nullptr, pQuery.get());

    ASSERT_EQ(0, query.error->find("order conversion error, node: @7\n'b'()@7 ASC"));
  }

  // failed nested function
  {
    sequence_function::deterministic_function_t fnFail = [](
      sequence_function::deterministic_buffer_t&,
      const sequence_function::deterministic_function_args_t&
    )->bool {
      return false;
    };
    order_function::contextual_function_t fnTest = [](
      order_function::contextual_buffer_t&,
      const std::locale& locale,
      void* cookie,
      const bool&,
      const order_function::contextual_function_args_t& args
    )->bool {
      iresearch::bstring buf;
      bool bNil;
      return args[0].value(buf, bNil, locale, cookie); // expect false from above
    };
    order_function order_function(fnTest);
    order_functions order_functions = {
      { "b", order_function },
    };
    sequence_function sequence_function(fnFail);
    sequence_functions sequence_functions = {
      { "c", sequence_function },
    };
    functions functions(sequence_functions, order_functions);
    auto query = query_builder(functions).build("name==A order b(c())", std::locale::classic());
    ASSERT_NE(nullptr, query.filter.get());
    ASSERT_EQ(nullptr, query.order);
    ASSERT_NE(nullptr, query.error);
    ASSERT_EQ(nullptr, query.limit);

    auto pQuery = query.filter->prepare(*reader);
    ASSERT_EQ(nullptr, pQuery.get());

    ASSERT_EQ(0, query.error->find("order conversion error, node: @9\n'b'('c'()@8)@9 ASC"));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
